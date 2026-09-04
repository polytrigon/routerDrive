#include "web_server.h"
#include "config.h"
#include "storage.h"
#include "wifi_portal.h"
#include "dxf2svg_js.h"
#include "style_css.h"
#include "led.h"

#include <WebServer.h>
#include <WiFi.h>
#include <FFat.h>
#include <FS.h>
#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

static WebServer server(80);
static File uploadFile;

// Set by GET /nowifi (see handleNoWifi()) - once true, handleIndex() shows
// the full page even while in AP/setup mode, for the rest of this boot.
// Starts false on every boot, same as apFullPageUnlocked's only reset
// path being a restart (which is also the only way back into a fresh
// AP-mode session in the first place).
static bool apFullPageUnlocked = false;

// A one-shot message shown at the top of the next page render, set by a
// POST handler right before its redirect to "/" (upload, delete, ...) and
// cleared as soon as renderPage() displays it - same pattern the old
// upload-only "uploadError" used, just generalized to cover successes too.
static String flashMessage;
static bool flashIsError = false;

// Names uploaded since the last time the file list was rendered - shown as
// a green checkmark next to those rows (see renderFilesSection()) so it's
// obvious which file(s) just landed, including when one overwrote an
// existing entry. Unlike flashMessage (overwritten by each individual
// /upload request), this ACCUMULATES across a whole multi-file batch -
// the JS upload flow sends one file per request but only reloads the page
// once at the end - and is cleared only once actually displayed.
static std::vector<String> justUploadedNames;

// Set by renderFilesSection() on every render and read right afterward by
// renderPage() to build the Upload section's folder dropdown, without
// mounting storage a second time in the same page load.
static std::vector<String> lastFolderList;
static String lastViewedFolder;

// Tracks every file part seen during one /upload POST (there can be more
// than one now that the form allows selecting multiple files at once), so
// handleUploadDone() can report an accurate "uploaded N files" - or list
// which ones failed - instead of a single pass/fail flag.
struct UploadBatch {
  std::vector<String> uploaded;
  std::vector<String> failed;
};
static UploadBatch uploadBatch;

static String joinNames(const std::vector<String> &names) {
  String out;
  for (size_t i = 0; i < names.size(); i++) {
    if (i > 0) out += ", ";
    out += names[i];
  }
  return out;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static String basenameOf(const String &path) {
  int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

// Path to a folder (root when folder is empty), no trailing slash except
// for the root "/" itself - e.g. "/" or "/projectA". Folders are a single
// level deep only: `folder` is always one plain name, never itself
// containing a "/".
static String folderDirPath(const String &folder) {
  String path = SVG_FOLDER;
  if (!path.endsWith("/")) {
    path += "/";
  }
  if (folder.length() > 0) {
    path += folder;
  }
  return path;
}

static String joinFolder(const String &folder, const String &basename) {
  String path = folderDirPath(folder);
  if (!path.endsWith("/")) {
    path += "/";
  }
  return path + basename;
}

static const int MAX_FOLDER_NAME_LEN = 24;

// Folder names are a single path segment the user typed (via the "+ New
// folder..." prompt) - keep them boring on purpose: letters, digits,
// spaces, "-", "_", nothing that could climb out of SVG_FOLDER or trip up
// FAT's shorter-name quirks. Re-checked server-side on every upload/delete
// even though the UI only ever offers names that already passed this.
static bool isValidFolderName(const String &name) {
  if (name.length() == 0 || name.length() > MAX_FOLDER_NAME_LEN) return false;
  if (name == "." || name == "..") return false;
  if (name[0] == ' ' || name[name.length() - 1] == ' ') return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

static String htmlEscape(const String &in) {
  String out = in;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

static String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + " B";
  }
  return String(bytes / 1024.0, 1) + " KB";
}

static String rssiQuality(int rssi) {
  if (rssi >= -50) return "Excellent";
  if (rssi >= -60) return "Good";
  if (rssi >= -70) return "Fair";
  return "Weak";
}

static String jsStringEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '\'') out += '\\';
    out += c;
  }
  return out;
}

static String urlEncode(const String &s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// FFat/FATFS timestamps only mean anything once NTP has actually synced
// (see wifi_portal.cpp's configTime() call) - before that, or for a device
// that's never had internet access, getLastWrite() returns a bogus
// near-zero epoch. 1600000000 (~Sept 2020) is a cheap floor well before
// this feature could ever produce a real timestamp, to tell the two apart.
static String formatDateTime(time_t t) {
  if (t < 1600000000) {
    return "-";
  }
  struct tm tmVal;
  localtime_r(&t, &tmVal);
  char buf[20];
  strftime(buf, sizeof(buf), "%m/%d/%Y", &tmVal);
  return String(buf);
}

// ---------------------------------------------------------------------------
// page rendering
// ---------------------------------------------------------------------------

struct FileEntry {
  String name;
  size_t size;
  time_t mtime;
  String cutType; // raw shaper:cutType value, e.g. "outside" - see readShaperInfo()
  String toolDia;  // raw shaper:toolDia value, e.g. "0.25 in"
};

// Every shape this app writes shaper:* attributes onto used to carry the
// same cutType/toolDia, back when applyShaperMetadata() only ever applied
// one chosen type to the whole file - but a file can now legitimately
// carry different values per shape: either edited per-line through the
// file list's cut editor, or uploaded straight from a real Origin export
// (which already varies cutType per path) with the upload form's own
// cutType left "unset" so nothing overwrote it. So this scans for
// every occurrence of the attribute, rather than stopping at the first,
// and reports "mixed" the moment a second, differing value shows up. Not
// a full XML parser: this app controls the exact attribute syntax it
// writes, so a plain-text scan for '<name>="value"' is enough - avoids
// pulling in an XML library just to look up two strings.
//
// This used to peek at only the first SHAPER_SCAN_LIMIT bytes of the
// file rather than reading the whole thing, to keep folder-listing
// memory use bounded on a board with limited SRAM already under pressure
// from Wi-Fi/WebServer buffers. That was fine back when
// applyShaperMetadata() stamped every shape with the same value right at
// upload time (so the attribute always showed up almost immediately).
// It quietly broke once per-line editing shipped: a real-hardware test
// edited two shapes deep into a multi-KB file and the file list showed
// "-"/"-" for both cut type AND bit size, even though the shapes'
// attributes were saved correctly and the editor itself displayed them
// fine on reopen (it reads the whole file via GET /svg, no size cap) -
// their attributes simply landed past the old 4KB window. Fixed below by
// streaming the file through a small fixed-size chunk buffer instead of
// a size-proportional one, so correctness no longer depends on file size
// while peak memory use is still bounded and, in fact, smaller than the
// old worst case (one ~2KB buffer instead of up to 4KB).
static const size_t SHAPER_SCAN_CHUNK = 2048;
// Every '<attr>="<value>"' this app ever writes is well under this many
// bytes (the longest, shaper:cutOffset="0.125 in", is ~27) - carrying
// this many bytes from the end of one chunk into the front of the next
// guarantees no occurrence is ever split across a chunk boundary, so
// each chunk can still be scanned independently with scanAttrMixed().
static const size_t SHAPER_SCAN_OVERLAP = 64;
// Sane ceiling on total bytes scanned per file, purely so one huge file
// can't make the whole folder listing crawl - well beyond any realistic
// converted SVG for this device's use case (a Shaper Studio export of a
// real multi-shape design was ~19 KB).
static const size_t SHAPER_SCAN_HARD_CAP = 262144;

// Scans hay for every occurrence of attrName="..." and reports whether
// two or more of them hold differing non-blank values. firstValue is set
// to whichever value is found first regardless of the mixed result, so
// callers get a usable single value in the common (non-mixed) case for
// free. Returns as soon as one differing value is found rather than
// counting every occurrence - the caller only needs to know "mixed or
// not", not how many distinct values there are.
static bool scanAttrMixed(const String &hay, const String &attrName, String &firstValue) {
  firstValue = "";
  String needle = attrName + "=\"";
  int searchFrom = 0;
  while (true) {
    int idx = hay.indexOf(needle, searchFrom);
    if (idx < 0) break;
    int start = idx + needle.length();
    int end = hay.indexOf('"', start);
    if (end < 0) break;
    String val = hay.substring(start, end);
    searchFrom = end + 1;
    if (val.length() == 0) continue;
    if (firstValue.length() == 0) {
      firstValue = val;
    } else if (val != firstValue) {
      return true;
    }
  }
  return false;
}

// Runs one already-read chunk of text through scanAttrMixed() for a
// single attribute name and merges the result into state (firstValue/
// mixed) that the caller carries across chunks - so "mixed" reflects the
// whole file scanned so far, not just this one chunk.
static void scanChunkForAttr(const String &chunk, const String &attrName, String &firstValue, bool &mixed) {
  if (mixed) return; // already known mixed, nothing left to learn
  String chunkFirst;
  bool chunkMixed = scanAttrMixed(chunk, attrName, chunkFirst);
  if (chunkFirst.length() == 0) return; // attribute not present in this chunk
  if (firstValue.length() == 0) {
    firstValue = chunkFirst;
  } else if (chunkFirst != firstValue) {
    mixed = true;
    return;
  }
  if (chunkMixed) mixed = true;
}

// Streams an already-open file handle (left at its current read position
// - call this before any other read on the same handle, and before the
// handle is closed) through a small fixed-size buffer, pulling out
// shaper:cutType/shaper:toolDia if present anywhere in it. A plain SVG
// never run through the cut-type feature (or uploaded with cut type left
// "unset" AND never edited per-line) leaves both blank - the file list
// shows "-" for those. cutType comes back as the sentinel raw value
// "mixed" (see cutTypeLabel()) when more than one distinct shaper:cutType
// value is present; toolDia comes back as the literal display string
// "Mixed" directly, since (unlike cutType) it's shown as-is with no
// separate label-mapping step.
static void readShaperInfo(File &f, String &cutType, String &toolDia) {
  cutType = "";
  toolDia = "";
  if (f.size() == 0) return;
  bool cutTypeMixed = false, toolDiaMixed = false;
  char *buf = new char[SHAPER_SCAN_CHUNK + 1];
  String overlapTail = "";
  size_t totalRead = 0;
  while (totalRead < SHAPER_SCAN_HARD_CAP) {
    size_t n = f.read((uint8_t *)buf, SHAPER_SCAN_CHUNK);
    if (n == 0) break; // end of file
    buf[n] = '\0';
    totalRead += n;
    String chunk = overlapTail + String(buf);
    scanChunkForAttr(chunk, "shaper:cutType", cutType, cutTypeMixed);
    scanChunkForAttr(chunk, "shaper:toolDia", toolDia, toolDiaMixed);
    if (cutTypeMixed && toolDiaMixed) break; // both already confirmed mixed
    overlapTail = (chunk.length() > SHAPER_SCAN_OVERLAP) ? chunk.substring(chunk.length() - SHAPER_SCAN_OVERLAP) : chunk;
  }
  delete[] buf;
  if (cutTypeMixed) cutType = "mixed";
  if (toolDiaMixed) toolDia = "Mixed";
}

// Display label for the file list's Cut type column - mirrors the Upload
// section's cutType <select> option text exactly (see renderPage()), plus
// the "mixed" sentinel readShaperInfo() sets when a file's shapes don't
// all agree on one cut type.
static String cutTypeLabel(const String &raw) {
  if (raw == "mixed") return "Mixed";
  if (raw == "outside") return "Outside";
  if (raw == "inside") return "Inside";
  if (raw == "pocket") return "Pocket";
  if (raw == "online") return "On Line";
  if (raw == "guide") return "Guide";
  return "-";
}

static bool nameContainsCI(const String &name, const String &needleLower) {
  String hay = name;
  hay.toLowerCase();
  return hay.indexOf(needleLower) >= 0;
}

// Builds the whole "Files" block: search box + Prev/Next pager (once there
// are more than FILES_PER_PAGE files - see config.h), and the table itself
// with name/size/upload-date/delete per row. Reads "q" (search) and "page"
// straight off the current request, the same way the other handlers below
// read server args directly, rather than threading them through as params.
static String renderFilesSection() {
  if (!storageBeginAppAccess()) {
    return "<h2>Files</h2><p style='color:#b00'>Could not read storage.</p>";
  }

  // Subfolders directly under SVG_FOLDER (one level only).
  std::vector<String> folders;
  {
    File topDir = FFat.open(SVG_FOLDER);
    if (topDir && topDir.isDirectory()) {
      File f = topDir.openNextFile();
      while (f) {
        if (f.isDirectory()) {
          String name = basenameOf(String(f.name()));
          // Skip OS-generated junk directories (macOS ".Trashes",
          // ".fseventsd", ".Spotlight-V100", etc. show up on any FAT/
          // exFAT volume a Mac has mounted) - never something a user
          // created through this UI, since isValidFolderName() forbids
          // a leading ".".
          if (!name.startsWith(".")) {
            folders.push_back(name);
          }
        }
        f.close();
        f = topDir.openNextFile();
      }
    }
    topDir.close();
  }
  std::sort(folders.begin(), folders.end());

  String viewFolder;
  if (server.hasArg("dir")) {
    viewFolder = server.arg("dir");
  }
  viewFolder.trim();
  bool folderExists = std::find(folders.begin(), folders.end(), viewFolder) != folders.end();
  if (viewFolder.length() > 0 && !folderExists) {
    viewFolder = ""; // unknown/stale folder (deleted, or a tampered link) - fall back to root
  }

  std::vector<FileEntry> entries;
  {
    File dir = FFat.open(folderDirPath(viewFolder));
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          FileEntry e;
          e.name = basenameOf(String(f.name()));
          e.size = f.size();
          e.mtime = f.getLastWrite();
          readShaperInfo(f, e.cutType, e.toolDia);
          entries.push_back(e);
        }
        f.close();
        f = dir.openNextFile();
      }
    }
    dir.close(); // must close all handles before storageEndAppAccess() unmounts
  }

  // Filenames per folder (root + every subfolder), for the upload JS's
  // overwrite-conflict check - it can target a different folder than the
  // one being browsed here via its own folder dropdown, so it needs every
  // folder's names, not just this page's.
  std::vector<String> folderKeys;
  std::vector<std::vector<String>> folderFiles;
  folderKeys.push_back("");
  for (size_t k = 0; k < folders.size(); k++) folderKeys.push_back(folders[k]);
  for (size_t k = 0; k < folderKeys.size(); k++) {
    std::vector<String> names;
    File fdir = FFat.open(folderDirPath(folderKeys[k]));
    if (fdir && fdir.isDirectory()) {
      File f = fdir.openNextFile();
      while (f) {
        if (!f.isDirectory()) names.push_back(basenameOf(String(f.name())));
        f.close();
        f = fdir.openNextFile();
      }
    }
    fdir.close();
    folderFiles.push_back(names);
  }

  size_t used = FFat.usedBytes();
  size_t total = FFat.totalBytes();
  storageEndAppAccess(false); // just reading, nothing changed - no need to nudge the host

  // Stashed for renderPage() to reuse when it builds the Upload section's
  // folder dropdown right after this call, without mounting storage again.
  lastFolderList = folders;
  lastViewedFolder = viewFolder;

  std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
    return a.name < b.name;
  });

  int totalCount = (int)entries.size();
  bool showControls = totalCount > FILES_PER_PAGE;

  String query;
  if (server.hasArg("q")) {
    query = server.arg("q");
  }
  query.trim();

  std::vector<FileEntry> filtered;
  if (query.length() > 0) {
    String needle = query;
    needle.toLowerCase();
    for (size_t i = 0; i < entries.size(); i++) {
      if (nameContainsCI(entries[i].name, needle)) {
        filtered.push_back(entries[i]);
      }
    }
  } else {
    filtered = entries;
  }

  int filteredCount = (int)filtered.size();
  int totalPages = filteredCount == 0 ? 1 : (filteredCount + FILES_PER_PAGE - 1) / FILES_PER_PAGE;
  int page = server.hasArg("page") ? server.arg("page").toInt() : 1;
  if (page < 1) page = 1;
  if (page > totalPages) page = totalPages;

  String html = "<h2>Files</h2>";

  // Folder nav: a dropdown (always shown, even with zero folders yet, so
  // "+ New folder..." is always reachable from here) navigates between
  // root and every existing folder. data-current lets the JS revert the
  // select if "+ New folder..." is cancelled or rejected. "Delete this
  // folder" lives down by the Delete/Move buttons below, not here - see
  // the .file-actions row further down.
  html += "<p class='sub'>";
  html += "<select id='dirNav' onchange='handleDirNavChange()' data-current='" + htmlEscape(viewFolder) + "'>";
  html += String("<option value=''") + (viewFolder.length() == 0 ? " selected" : "") + ">Folder: HOME</option>";
  for (size_t i = 0; i < folders.size(); i++) {
    bool sel = (folders[i] == viewFolder);
    html += "<option value='" + htmlEscape(folders[i]) + "'" + (sel ? " selected" : "") + ">Folder: " + htmlEscape(folders[i]) + "</option>";
  }
  html += "<option value='__new__'>+ New folder...</option>";
  html += "</select>";
  // Hidden form for "+ New folder..." (see handleDirNavChange in the page
  // script) - a real POST + server-side 303 redirect, same pattern as every
  // other mutation on this page, rather than a fetch() that a plain-form
  // ESP32 WebServer round trip is more reliably GET-able afterward.
  html += "<form id='mkdirForm' method='POST' action='/mkdir' style='display:none'>"
          "<input type='hidden' name='name' id='mkdirName'></form>";
  html += "</p>";

  if (showControls) {
    html += "<form method='GET' action='/' class='search'>";
    if (viewFolder.length() > 0) {
      html += "<input type='hidden' name='dir' value='" + htmlEscape(viewFolder) + "'>";
    }
    html += "<input type='text' name='q' placeholder='Search files...' value='" + htmlEscape(query) + "'> ";
    html += "<button type='submit'>Search</button>";
    if (query.length() > 0) {
      String clearHref = viewFolder.length() > 0 ? ("/?dir=" + urlEncode(viewFolder)) : "/";
      html += " <a href='" + clearHref + "'>Clear</a>";
    }
    html += "</form>";
  }

  // One-shot: whatever's in justUploadedNames belongs to the batch that
  // just finished (if any) - consume it now so a later render (e.g. after
  // a plain page refresh) doesn't keep re-marking the same rows forever.
  std::vector<String> justUploaded = justUploadedNames;
  justUploadedNames.clear();

  // One form wraps the whole table so any number of checked rows can be
  // deleted in a single POST - see handleDelete(), which now loops over
  // every repeated "name" field instead of assuming exactly one. The
  // hidden "dir" field tells handleDelete() which folder these names
  // actually live in.
  html += "<form id='deleteForm' method='POST' action='/delete'>";
  if (viewFolder.length() > 0) {
    html += "<input type='hidden' name='dir' value='" + htmlEscape(viewFolder) + "'>";
  }
  html += "<table><tr><th><input type='checkbox' id='selectAllFiles' onclick='toggleAllFiles(this)'></th><th>Name</th><th>Size</th><th>Uploaded</th><th>Cut type</th><th>Bit size</th></tr>";
  if (filteredCount == 0) {
    html += String("<tr><td colspan=6><em>") + (query.length() > 0 ? "No files match your search." : "No files yet.") + "</em></td></tr>";
  } else {
    int startIdx = (page - 1) * FILES_PER_PAGE;
    int endIdx = min(filteredCount, startIdx + FILES_PER_PAGE);
    for (int i = startIdx; i < endIdx; i++) {
      const FileEntry &e = filtered[i];
      bool isNew = std::find(justUploaded.begin(), justUploaded.end(), e.name) != justUploaded.end();
      String checkmark = isNew ? " <span style='color:#0a0' title='Just uploaded'>&#10003;</span>" : "";
      html += "<tr><td><input type='checkbox' class='rowcheck' name='name' value='" + htmlEscape(e.name) + "' onchange='updateBatchButtons()'></td>";
      html += "<td>" + htmlEscape(e.name) + "</td><td>" + formatBytes(e.size) + "</td><td>" + formatDateTime(e.mtime) + checkmark + "</td>"
              "<td><button type='button' class='link-btn cutTypeCell' data-name='" + htmlEscape(e.name) + "' data-dir='" + htmlEscape(viewFolder) +
              "' onclick='openCutEditorFromBtn(this)'>" + cutTypeLabel(e.cutType) + "</button></td>"
              "<td>" + (e.toolDia.length() > 0 ? htmlEscape(e.toolDia) : "-") + "</td></tr>";
    }
  }
  html += "<tr><td colspan=6 style='color:#666'>" + formatBytes(used) + " used of " + formatBytes(total) + "</td></tr>";
  html += "</table>";
  // A move destination exists whenever there's somewhere other than the
  // currently-viewed folder to put files: always true once you're inside
  // any folder (root is always a valid destination), and true at root
  // once at least one folder exists.
  bool hasMoveDest = viewFolder.length() > 0 || !folders.empty();
  bool showDeleteFolder = viewFolder.length() > 0;
  if (filteredCount > 0 || showDeleteFolder) {
    // Delete/Move start disabled - updateBatchButtons() (in the page
    // script) enables them once at least one row is checked. "Delete
    // this folder" doesn't depend on any row being checked (it acts on
    // the folder itself), so it's laid out separately via .file-actions
    // (left group: Delete/Move; right group: Delete this folder) rather
    // than sharing their disabled-until-selected state, and is rendered
    // even when the folder is empty (filteredCount == 0) so an empty
    // folder can still be removed from here.
    html += "<div class='file-actions'><div>";
    if (filteredCount > 0) {
      html += "<button type='submit' id='deleteBtn' formaction='/delete' onclick='return confirmBatchDelete()' disabled>Delete</button>";
      if (hasMoveDest) {
        // "Move" no longer submits directly - it just reveals #movePanel
        // below (destination select + confirm), so the always-visible
        // "Move to:" dropdown doesn't sit in the way when nobody's
        // moving anything.
        html += " <button type='button' id='moveBtn' onclick='showMovePanel()' disabled>Move</button>";
      }
    }
    html += "</div>";
    if (showDeleteFolder) {
      html += "<button type='submit' formaction='/deletefolder' class='link-btn' onclick='return confirmDeleteFolder()'>Delete this folder</button>";
    }
    html += "</div>";
  }
  html += "</form>";
  if (filteredCount > 0 && hasMoveDest) {
    // Rendered outside <form id='deleteForm'> (below it in the DOM, not
    // nested inside), so both the destination select and the confirm
    // button carry an explicit form='deleteForm' attribute - HTML lets a
    // control outside a <form> still submit with/into it that way, which
    // is what makes the checked rowchecks (and the chosen dest) travel
    // together in the one POST, same formaction-override trick already
    // used for Delete/Move selected above.
    html += "<div id='movePanel' style='display:none'>";
    html += "<p class='sub'>Move selected file(s) to:</p>";
    html += "<select name='dest' id='moveDest' form='deleteForm'>";
    if (viewFolder.length() > 0) {
      html += "<option value=''>HOME</option>";
    }
    for (size_t i = 0; i < folders.size(); i++) {
      if (folders[i] == viewFolder) continue;
      html += "<option value='" + htmlEscape(folders[i]) + "'>" + htmlEscape(folders[i]) + "</option>";
    }
    html += "</select> ";
    html += "<button type='submit' id='confirmMoveBtn' form='deleteForm' formaction='/move' onclick='return confirmMove()'>Confirm</button> ";
    html += "<button type='button' id='cancelMoveBtn' onclick=\"document.getElementById('movePanel').style.display='none'\">Cancel</button>";
    html += "</div>";
  }

  if (showControls && totalPages > 1) {
    String qParam = query.length() > 0 ? ("&q=" + urlEncode(query)) : "";
    String dParam = viewFolder.length() > 0 ? ("&dir=" + urlEncode(viewFolder)) : "";
    html += "<p class='pager'>";
    if (page > 1) {
      html += "<a href='/?page=" + String(page - 1) + qParam + dParam + "'>&laquo; Prev</a> ";
    }
    html += "Page " + String(page) + " of " + String(totalPages) + " ";
    if (page < totalPages) {
      html += "<a href='/?page=" + String(page + 1) + qParam + dParam + "'>Next &raquo;</a>";
    }
    html += "</p>";
  }

  // Filenames per folder, for the overwrite-warning check in both upload
  // paths below - keyed by folder name ("" = root) so the check works no
  // matter which folder the upload dropdown targets, independent of
  // whichever folder is currently being browsed above.
  html += "<script>var existingFilesByFolder = {";
  for (size_t k = 0; k < folderKeys.size(); k++) {
    if (k > 0) html += ",";
    html += "'" + jsStringEscape(folderKeys[k]) + "':[";
    const std::vector<String> &names = folderFiles[k];
    for (size_t i = 0; i < names.size(); i++) {
      if (i > 0) html += ",";
      html += "'" + jsStringEscape(names[i]) + "'";
    }
    html += "]";
  }
  html += "};</script>";

  return html;
}

// A small "built <date> <time>" line, shown at the bottom of every page.
// Uses the compiler's own __DATE__/__TIME__ macros (the moment this file
// was compiled in Arduino IDE) rather than a manually-maintained version
// number, so it can't go stale or get forgotten on some future edit - it
// always reflects exactly what's actually running on the device, which is
// the point: an easy way to confirm you're looking at the build you think
// you just flashed.
// Credit line + build stamp. Styling (font size, color, spacing from the
// rest of the page) lives entirely on the <footer> wrapper in style.css,
// not here - callers below wrap this (and, on the full page, the LED
// toggle) in <footer>...</footer>, which is also what anchors it to the
// bottom of the page (see style.css's body/footer rules).
static String renderBuildFooter() {
  String html = "<p>RouterDrive - By ";
  html += "<a href='https://www.instagram.com/tseng.co/' target='_blank' rel='noopener'>MTseng</a>";
  html += " &amp; Claude - Built ";
  html += __DATE__;
  html += " ";
  html += __TIME__;
  html += "</p>";
  // Nominative-fair-use disclaimer: RouterDrive references Shaper/Festool
  // by name throughout (it's built specifically for the Origin) without
  // being made, endorsed, or sponsored by either company - this line says
  // so plainly. Shown on every page the footer appears on, including the
  // AP-mode setup page, not just the full one.
  html += "<p style='font-size:.9em'>RouterDrive is an independent, unofficial project and is not affiliated "
          "with, endorsed by, or sponsored by Shaper Tools, Festool, or their parent companies.</p>";
  return html;
}

// A small, deliberately low-key toggle for the status LED (see led.h) -
// only shown on the full page, not the streamlined captive-window one,
// so it doesn't add to that page's already-minimal footprint. Styled via
// the .link-btn class (style.css) to look like a plain text link rather
// than one of the page's normal blue buttons.
static String renderLedToggle() {
  String html = "<p>";
  html += "<form method='POST' action='/led-toggle' style='display:inline;margin:0'>";
  html += "<button type='submit' class='link-btn'>";
  html += ledIsEnabled() ? "Turn status LED off" : "Turn status LED on";
  html += "</button></form></p>";
  return html;
}

// Shared by the normal page's Wi-Fi section and the streamlined captive-
// window page below, so the two forms can't drift out of sync.
static String renderWifiCredentialsForm(const String &submitLabel) {
  String html;
  html += "<form method='POST' action='/wifi'>";
  html += "<label>Network name (SSID)<br><input name='ssid' required></label><br><br>";
  html += "<label>Password<br><input name='password' type='password'></label><br><br>";
  html += "<button type='submit'>" + submitLabel + "</button>";
  html += "</form>";
  return html;
}

static String renderWifiSection() {
  String html;
  if (wifiPortalState() == WIFI_STATE_AP) {
    html += "<h2>Wi-Fi setup</h2>";
    html += renderWifiCredentialsForm("Save &amp; connect");
  } else {
    html += "<h2>Wi-Fi</h2><p>" + htmlEscape(wifiPortalStatusText()) + "</p>";
    html += "<details><summary>Change network</summary>";
    html += renderWifiCredentialsForm("Save &amp; reconnect");
    html += "<form method='POST' action='/wifi-reset' onsubmit=\"return confirm('Forget saved Wi-Fi and restart into setup mode?')\">";
    html += "<button type='submit'>Forget Wi-Fi</button></form>";
    html += "</details>";
  }
  return html;
}

// Used to be gated on isCaptiveAssistant(), a User-Agent heuristic trying
// to guess whether a request was coming from macOS/iOS's captive-window
// popup specifically. Real-hardware testing showed that guess was
// unreliable even after a couple of rounds of tuning against real Serial
// Monitor data (see README/project notes) - and it never needed to be a
// guess in the first place: wifiPortalState() already tells us for a fact
// whether we're in AP/setup mode. So as of 2026-09-03, this page is shown
// for every request while in AP mode, full stop, regardless of what's
// asking - popup, a real browser tab, any platform. See handleIndex() and
// "Prefer to skip this?" below for how to still reach the full page while
// staying on the setup hotspot.
static String renderCaptivePage() {
  String apIp = WiFi.softAPIP().toString();
  String html;
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>RouterDrive</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "</head><body>";
  html += "<div class='page'>";
  html += "<h1>RouterDrive</h1><p class='sub'>Wireless Filesystem for the Origin</p>";
  html += "<h2>Join your Wi-Fi</h2>";
  html += "<p class='sub'>Enter your home Wi-Fi network below. RouterDrive will restart and connect to it, so "
          "you can reach it from anywhere on that network afterward.</p>";
  html += renderWifiCredentialsForm("Save &amp; connect");
  html += "<p class='sub'>Once it's connected, join that same Wi-Fi network yourself and visit "
          "<b>http://" + String(HOSTNAME) + ".local/</b> in any browser to reach RouterDrive from there.</p>";
  html += "<h2>Prefer to skip this?</h2>";
  html += "<p class='sub'>You don't have to join a Wi-Fi network to use RouterDrive. Close this window, then in "
          "your own browser (Safari, Chrome, etc. - not this popup) open <b>http://" + String(HOSTNAME) +
          ".local/nowifi</b> (or <b>http://" + apIp + "/nowifi</b> if that address doesn't work) while you're "
          "still connected to the \"" + String(PORTAL_SSID) + "\" network - that gets you the full page, "
          "including file uploads and the DXF converter, staying joined to this hotspot instead of your normal "
          "Wi-Fi. You only need to do this once per restart - after that this device keeps showing the full "
          "page here until it's restarted or joins a real Wi-Fi network.</p>";
  html += "</div>"; // .page
  html += "<footer>";
  html += renderBuildFooter();
  html += "</footer>";
  html += "</body></html>";
  return html;
}

static String renderPage() {
  String html;
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>RouterDrive</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "</head><body>";
  html += "<div class='page'>";
  html += "<h1>RouterDrive</h1><p class='sub'>Wireless Filesystem for the Origin</p>";

  html += "<p class='status'>";
  if (usbIsConnected()) {
    html += "<span class='dot dot-ok'></span>USB connected";
  } else {
    html += "<span class='dot dot-bad'></span>USB not connected";
  }
  html += " &nbsp;&nbsp; ";
  if (wifiPortalState() == WIFI_STATE_STA) {
    html += "<span class='dot dot-ok'></span>Wi-Fi: " + htmlEscape(WiFi.SSID()) +
            " (" + rssiQuality(wifiPortalRSSI()) + ")";
  } else {
    html += "<span class='dot dot-bad'></span>Wi-Fi: setup mode";
  }
  html += "</p>";

  if (flashMessage.length() > 0) {
    String color = flashIsError ? "#b00" : "#080";
    String prefix = flashIsError ? "" : "&#10003; "; // checkmark for success
    html += "<p style='color:" + color + "'>" + prefix + htmlEscape(flashMessage) + "</p>";
    flashMessage = "";
  }

  html += renderFilesSection();

  html += "<h2>Upload files</h2>";
  html += "<p class='sub'>Select DXF and/or SVG files, mixed together if you like. DXFs are automatically "
          "converted to the SVG format the Origin requires (handles most shapes, including text). Optionally "
          "pick a folder to upload into, and blanket-assign a Shaper cut type, depth, and tool diameter to "
          "each file below.</p>";
  html += "<input type='file' id='uploadFile' accept='.dxf,.svg' multiple>";
  html += "<br><br>";
  html += "<select id='dxfUnit' class='full-width'><option value='mm' selected>Units: mm</option><option value='in'>Units: inches</option></select>";
  html += "<br><br>";
  html += "<select id='uploadFolder' class='full-width' onchange='handleFolderSelectChange()'>";
  html += String("<option value=''") + (lastViewedFolder.length() == 0 ? " selected" : "") + ">Folder: HOME</option>";
  for (size_t i = 0; i < lastFolderList.size(); i++) {
    bool sel = (lastFolderList[i] == lastViewedFolder);
    html += "<option value='" + htmlEscape(lastFolderList[i]) + "'" + (sel ? " selected" : "") + ">Folder: " + htmlEscape(lastFolderList[i]) + "</option>";
  }
  html += "<option value='__new__'>+ New folder...</option>";
  html += "</select>";
  html += "<br><br>";
  html += "<select id='cutType' class='full-width' onchange='toggleToolDiaRow()'>"
          "<option value='' selected>Cut type: unset</option>"
          "<option value='outside'>Cut type: Outside</option>"
          "<option value='inside'>Cut type: Inside</option>"
          "<option value='pocket'>Cut type: Pocket</option>"
          "<option value='online'>Cut type: On Line</option>"
          "<option value='guide'>Cut type: Guide</option>"
          "</select>";
  html += "<br><br>";
  html += "<div id='toolDiaWrap' style='display:none'>"
          "<select id='toolDiaPreset' class='full-width' onchange='handleToolDiaPresetChange()'>"
          "<option value='' selected>Tool diameter: choose one (required)</option>"
          "<option value='0.125|in'>Tool diameter: 1/8 in</option>"
          "<option value='0.25|in'>Tool diameter: 1/4 in</option>"
          "<option value='0.375|in'>Tool diameter: 3/8 in</option>"
          "<option value='0.5|in'>Tool diameter: 1/2 in</option>"
          "<option value='3|mm'>Tool diameter: 3 mm</option>"
          "<option value='6|mm'>Tool diameter: 6 mm</option>"
          "<option value='8|mm'>Tool diameter: 8 mm</option>"
          "<option value='10|mm'>Tool diameter: 10 mm</option>"
          "<option value='__custom__'>Tool diameter: Custom...</option>"
          "</select>"
          "<br><br>"
          "<div id='toolDiaCustomWrap' style='display:none'>"
          "<div class='depth-row'>"
          "<input type='number' id='toolDiaCustom' step='0.001' min='0' placeholder='Custom tool diameter'>"
          "<select id='toolDiaUnit'><option value='mm' selected>mm</option><option value='in'>inches</option></select>"
          "</div>"
          "<br><br>"
          "</div>"
          "</div>";
  html += "<div class='depth-row'>"
          "<input type='number' id='cutDepth' step='0.001' min='0' placeholder='Cut depth (optional)'>"
          "<select id='cutDepthUnit'><option value='mm' selected>mm</option><option value='in'>inches</option></select>"
          "</div>";
  html += "<br>";
  html += "<button type='button' id='uploadBtn'>Convert &amp; upload</button>";
  html += "<p id='uploadStatus' class='sub'></p>";

  html += "<h2>Restart to update Origin's import list</h2>";
  html += "<p class='sub'>Once you've uploaded files, you'll need to restart RouterDrive before they propegate "
          "in the Shaper Origin's import list. Click the Restart button below - it takes a few seconds and "
          "drops Wi-Fi briefly, but this page reconnects on its own once it's back. If restart ever doesn't do "
          "it, a physical unplug/replug of the USB cable always will.</p>";
  html += "<form method='POST' action='/rescan' onsubmit=\"return confirm('Restart RouterDrive? Wi-Fi will drop for a few seconds.')\"><button type='submit'>Restart RouterDrive</button></form>";

  html += renderWifiSection();

  // Per-line cut editor: hidden by default, opened from a "Cut type" cell
  // button in the file list above (see cutTypeCell/openCutEditorFromBtn).
  // Fetches the real file's SVG from the new /svg route, renders it, and
  // lets the user click individual paths to give them their own cut
  // type/depth/tool diameter instead of the whole-file value the Upload
  // section's own controls apply. Static markup - openCutEditor() fills
  // in the title and injects the fetched SVG at open time.
  html += "<div id='cutEditorOverlay' class='modal-overlay' style='display:none'>"
          "<div class='modal-box cut-editor'>"
          "<div class='cut-editor-header'>"
          "<h3 id='cutEditorTitle'>Edit cut types</h3>"
          "<button type='button' class='link-btn' onclick='closeCutEditor()'>Close</button>"
          "</div>"
          "<p class='sub'>Click a line to select it. Shift-click to select more than one. Everything "
          "you select shares whatever you set below.</p>"
          "<div class='cut-editor-body'>"
          "<div class='cut-editor-viewer'>"
          "<div id='cutEditorSvgWrap' class='cut-editor-svg-wrap'><p class='sub'>Loading...</p></div>"
          "<div class='cut-legend'>"
          "<div><span class='swatch' style='background:#1437c9'></span>Outside</div>"
          "<div><span class='swatch' style='background:#0a8a3f'></span>Inside</div>"
          "<div><span class='swatch' style='background:#8a2be2'></span>Pocket</div>"
          "<div><span class='swatch' style='background:#888'></span>On Line</div>"
          "<div><span class='swatch' style='background:#c9950a'></span>Guide</div>"
          "<div><span class='swatch' style='background:#000'></span>Not set yet</div>"
          "<div><span class='swatch' style='background:#00b8ff'></span>Selected</div>"
          "</div>" // .cut-legend
          "</div>" // .cut-editor-viewer
          "<div class='cut-editor-panel'>"
          "<p id='cutEditorSelCount' class='sub'>No line selected.</p>"
          "<select id='editCutType' class='full-width' onchange='toggleEditToolDiaWrap()'>"
          "<option value='' selected>Cut type: choose one</option>"
          "<option value='outside'>Cut type: Outside</option>"
          "<option value='inside'>Cut type: Inside</option>"
          "<option value='pocket'>Cut type: Pocket</option>"
          "<option value='online'>Cut type: On Line</option>"
          "<option value='guide'>Cut type: Guide</option>"
          "</select>"
          "<br><br>"
          "<div id='editToolDiaWrap' style='display:none'>"
          "<select id='editToolDiaPreset' class='full-width' onchange='handleEditToolDiaPresetChange()'>"
          "<option value='' selected>Tool diameter: choose one (required)</option>"
          "<option value='0.125|in'>Tool diameter: 1/8 in</option>"
          "<option value='0.25|in'>Tool diameter: 1/4 in</option>"
          "<option value='0.375|in'>Tool diameter: 3/8 in</option>"
          "<option value='0.5|in'>Tool diameter: 1/2 in</option>"
          "<option value='3|mm'>Tool diameter: 3 mm</option>"
          "<option value='6|mm'>Tool diameter: 6 mm</option>"
          "<option value='8|mm'>Tool diameter: 8 mm</option>"
          "<option value='10|mm'>Tool diameter: 10 mm</option>"
          "<option value='__custom__'>Tool diameter: Custom...</option>"
          "</select>"
          "<br><br>"
          "<div id='editToolDiaCustomWrap' style='display:none'>"
          "<div class='depth-row'>"
          "<input type='number' id='editToolDiaCustom' step='0.001' min='0' placeholder='Custom tool diameter'>"
          "<select id='editToolDiaUnit'><option value='mm' selected>mm</option><option value='in'>inches</option></select>"
          "</div>"
          "<br><br>"
          "</div>"
          "</div>"
          "<div class='depth-row'>"
          "<input type='number' id='editDepth' step='0.001' min='0' placeholder='Cut depth (optional)'>"
          "<select id='editDepthUnit'><option value='mm' selected>mm</option><option value='in'>inches</option></select>"
          "</div>"
          "<br>"
          "<button type='button' id='applyToSelectedBtn' disabled onclick='applyToSelectedShapes()'>Apply to selected</button>"
          "</div>" // .cut-editor-panel
          "</div>" // .cut-editor-body
          "<p id='cutEditorStatus' class='sub'></p>"
          "<button type='button' id='saveCutEditorBtn' onclick='saveCutEditor()'>Save changes</button> "
          "<button type='button' class='link-btn' onclick='closeCutEditor()'>Cancel</button>"
          "</div>" // .cut-editor
          "</div>"; // .modal-overlay

  html += "<script src='/dxf2svg.js'></script>";
  html += "<script>"
          "function toggleAllFiles(cb) {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck');"
          "for (var i = 0; i < boxes.length; i++) boxes[i].checked = cb.checked;"
          "updateBatchButtons();"
          "}"
          "function updateBatchButtons() {"
          "var anyChecked = document.querySelectorAll('#deleteForm .rowcheck:checked').length > 0;"
          "var delBtn = document.getElementById('deleteBtn');"
          "var moveBtn = document.getElementById('moveBtn');"
          "if (delBtn) delBtn.disabled = !anyChecked;"
          "if (moveBtn) moveBtn.disabled = !anyChecked;"
          "}"
          "function confirmDeleteFolder() {"
          "return confirm('Deleting this folder will also delete all of the contents. Continue?');"
          "}"
          "function confirmBatchDelete() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to delete.'); return false; }"
          "if (boxes.length === 1) return confirm('Delete ' + boxes[0].value + '?');"
          "return confirm('Delete ' + boxes.length + ' files?');"
          "}"
"function showMovePanel() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to move.'); return; }"
          "document.getElementById('movePanel').style.display = '';"
          "}"
          "function confirmMove() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to move.'); return false; }"
          "var destSel = document.getElementById('moveDest');"
          "var destLabel = destSel.options[destSel.selectedIndex].textContent;"
          "return confirm('Move ' + boxes.length + ' file(s) to ' + destLabel + '?');"
          "}"
          // dirNav comes from the Files section above - navigating just
          // follows a link (GET), but "+ New folder..." needs a real POST
          // to actually create the directory before there's anywhere to
          // navigate to, so it's handled separately here rather than as
          // a plain <option value>.
          "function handleDirNavChange() {"
          "var sel = document.getElementById('dirNav');"
          "if (sel.value === '__new__') {"
          "var name = prompt('New folder name (letters, numbers, spaces, - and _, up to 24 characters):');"
          "if (name) { name = name.trim(); }"
          "var valid = name && /^[A-Za-z0-9 _-]{1,24}$/.test(name) && name !== '.' && name !== '..' && name[0] !== ' ' && name[name.length - 1] !== ' ';"
          "if (!valid) {"
          "if (name) alert('That folder name is not allowed - use letters, numbers, spaces, - and _, up to 24 characters.');"
          "sel.value = sel.dataset.current;"
          "return;"
          "}"
"document.getElementById('mkdirName').value = name;"
          "document.getElementById('mkdirForm').submit();"
          "return;"
          "}"
          "location.href = sel.value ? ('/?dir=' + encodeURIComponent(sel.value)) : '/';"
          "}"
          // existingFilesByFolder comes from the <script> the Files section
          // emits, earlier in this same page - keyed by folder name ("" = root).
          "function findOverwriteConflicts(names, folder) {"
          "var list = existingFilesByFolder[folder] || [];"
          "var conflicts = [];"
          "for (var i = 0; i < names.length; i++) {"
          "if (list.indexOf(names[i]) !== -1) conflicts.push(names[i]);"
          "}"
          "return conflicts;"
          "}"
          "function handleFolderSelectChange() {"
          "var sel = document.getElementById('uploadFolder');"
          "if (sel.value === '__new__') {"
          "var name = prompt('New folder name (letters, numbers, spaces, - and _, up to 24 characters):');"
          "if (name) { name = name.trim(); }"
          "var valid = name && /^[A-Za-z0-9 _-]{1,24}$/.test(name) && name !== '.' && name !== '..' && name[0] !== ' ' && name[name.length - 1] !== ' ';"
          "if (!valid) {"
          "if (name) alert('That folder name is not allowed - use letters, numbers, spaces, - and _, up to 24 characters.');"
          "sel.value = sel.dataset.prev || '';"
          "return;"
          "}"
          "var found = false;"
          "for (var i = 0; i < sel.options.length; i++) {"
          "if (sel.options[i].value === name) { found = true; break; }"
          "}"
          "if (!found) {"
          "var opt = document.createElement('option');"
          "opt.value = name;"
          "opt.textContent = 'Folder: ' + name;"
          "sel.insertBefore(opt, sel.options[sel.options.length - 1]);"
          "}"
          "sel.value = name;"
          "}"
          "sel.dataset.prev = sel.value;"
          "}"
          "function toggleToolDiaRow() {"
          "var ct = document.getElementById('cutType').value;"
          "var needsOffset = (ct === 'outside' || ct === 'inside' || ct === 'pocket');"
          "document.getElementById('toolDiaWrap').style.display = needsOffset ? '' : 'none';"
          "}"
          "function handleToolDiaPresetChange() {"
          "var isCustom = document.getElementById('toolDiaPreset').value === '__custom__';"
          "document.getElementById('toolDiaCustomWrap').style.display = isCustom ? '' : 'none';"
          "}"
          "function applyShaperMetadata(svgText, cutType, depthVal, depthUnit, toolDiaVal, toolDiaUnit) {"
          "var doc = new DOMParser().parseFromString(svgText, 'image/svg+xml');"
          "if (doc.querySelector('parsererror')) { throw new Error('Could not parse SVG'); }"
          "var svgEl = doc.documentElement;"
          "var SHAPER_NS = 'http://www.shapertools.com/namespaces/shaper';"
          "var XMLNS_NS = 'http://www.w3.org/2000/xmlns/';"
          "svgEl.setAttributeNS(XMLNS_NS, 'xmlns:shaper', SHAPER_NS);"
          "var depthAttr = null;"
          "if (depthVal !== '' && depthVal !== null && !isNaN(parseFloat(depthVal))) {"
          "depthAttr = parseFloat(depthVal) + ' ' + depthUnit;"
          "}"
          "var needsOffset = (cutType === 'outside' || cutType === 'inside' || cutType === 'pocket');"
          "var toolDiaAttr = null;"
          "if (needsOffset && toolDiaVal !== '' && toolDiaVal !== null && toolDiaVal !== undefined && !isNaN(parseFloat(toolDiaVal))) {"
          "toolDiaAttr = parseFloat(toolDiaVal) + ' ' + toolDiaUnit;"
          "}"
          "var shapes = svgEl.querySelectorAll('path,rect,circle,ellipse,polygon,polyline,line');"
          "for (var i = 0; i < shapes.length; i++) {"
          "if (cutType) shapes[i].setAttributeNS(SHAPER_NS, 'shaper:cutType', cutType);"
          "if (depthAttr) shapes[i].setAttributeNS(SHAPER_NS, 'shaper:cutDepth', depthAttr);"
          "if (toolDiaAttr) {"
          "shapes[i].setAttributeNS(SHAPER_NS, 'shaper:toolDia', toolDiaAttr);"
          "shapes[i].setAttributeNS(SHAPER_NS, 'shaper:cutOffset', '0' + toolDiaUnit);"
          "}"
          "}"
          "return new XMLSerializer().serializeToString(doc);"
          "}"
          // -----------------------------------------------------------
          // Per-line cut editor (file list "Cut type" cell -> modal).
          // Lets the user open an already-uploaded file, click individual
          // paths, and give the selection its own cut type/depth/tool
          // diameter - independent of the Upload section's whole-file
          // applyShaperMetadata() above, which this deliberately does not
          // touch or reuse (it writes every shape uniformly; this writes
          // only the current selection). Saving re-POSTs the edited SVG
          // to the same, already-hardware-tested /upload route (same
          // filename + folder, so it overwrites in place) rather than a
          // new save endpoint.
          // -----------------------------------------------------------
          "var EDITOR_SHAPER_NS = 'http://www.shapertools.com/namespaces/shaper';"
          "var EDITOR_XMLNS_NS = 'http://www.w3.org/2000/xmlns/';"
          "var EDITOR_CUT_COLORS = {outside: '#1437c9', inside: '#0a8a3f', pocket: '#8a2be2', online: '#888', guide: '#c9950a'};"
          "var EDITOR_UNSET_COLOR = '#000';"
          "var EDITOR_SELECT_COLOR = '#00b8ff';"
          "var cutEditorState = {name: '', folder: '', svgEl: null, selected: [], dirty: false};"
          "function openCutEditorFromBtn(btn) {"
          "openCutEditor(btn.getAttribute('data-name'), btn.getAttribute('data-dir'));"
          "}"
          "function openCutEditor(name, folder) {"
          "cutEditorState = {name: name, folder: folder, svgEl: null, selected: [], dirty: false};"
          "document.getElementById('cutEditorTitle').textContent = 'Edit cut types: ' + name;"
          "document.getElementById('cutEditorSvgWrap').innerHTML = '<p class=\"sub\">Loading...</p>';"
          "document.getElementById('cutEditorStatus').textContent = '';"
          "document.getElementById('editCutType').value = '';"
          "toggleEditToolDiaWrap();"
          "updateSelectionSummary();"
          "document.getElementById('cutEditorOverlay').style.display = 'flex';"
          "var url = '/svg?name=' + encodeURIComponent(name) + (folder ? '&dir=' + encodeURIComponent(folder) : '');"
          "fetch(url).then(function(r) {"
          "if (!r.ok) throw new Error('Could not load file (HTTP ' + r.status + ')');"
          "return r.text();"
          "}).then(function(text) {"
          "var doc = new DOMParser().parseFromString(text, 'image/svg+xml');"
          "if (doc.querySelector('parsererror')) throw new Error('Could not parse this file as SVG.');"
          "var svgEl = doc.documentElement;"
          "svgEl.setAttributeNS(EDITOR_XMLNS_NS, 'xmlns:shaper', EDITOR_SHAPER_NS);"
          // CSS width/height (set via .style below) already take priority
          // over the SVG's own width/height *attributes* for on-screen
          // rendering wherever this element is embedded - removing the
          // attributes outright was unnecessary AND wrong: svgEl is the
          // same live node saveCutEditor() later clones and writes back
          // to the file, so deleting these attributes here meant every
          // saved file silently lost its real physical width/height,
          // leaving only viewBox behind. Leave them in place; only the
          // preview's on-screen size is overridden, not the saved data.
          "svgEl.style.width = '100%';"
          "svgEl.style.height = 'auto';"
          "svgEl.style.maxHeight = '55vh';"
          "svgEl.style.display = 'block';"
          "var wrap = document.getElementById('cutEditorSvgWrap');"
          "wrap.innerHTML = '';"
          "wrap.appendChild(svgEl);"
          // Note: appendChild() here *moves* svgEl out of the parsed
          // `doc` and into the live page - `doc` is left hollow after
          // this (no documentElement), so anything that needs to
          // serialize the edited SVG later (see saveCutEditor()) must
          // clone svgEl itself, not this now-empty doc.
          "cutEditorState.svgEl = svgEl;"
          "var shapes = svgEl.querySelectorAll('path,rect,circle,ellipse,polygon,polyline,line');"
          "for (var i = 0; i < shapes.length; i++) cutEditorInitShape(shapes[i]);"
          "}).catch(function(err) {"
          "document.getElementById('cutEditorSvgWrap').innerHTML = '<p style=\"color:#b00\">' + err.message + '</p>';"
          "});"
          "}"
          // A thin, unfilled cut line is a poor click target - only the
          // stroke pixels themselves are hit-testable by default, and the
          // hollow interior of a closed shape like a box is empty space
          // that falls through to whatever's behind it. So each real shape
          // gets an invisible, much-wider clone layered directly on top,
          // used only for hit-testing - the visible shape underneath stays
          // thin and correctly colored, while clicking anywhere near the
          // line (not just exactly on its thin pixel) selects it. The
          // clone is marked data-hit-proxy so saveCutEditor() can leave it
          // out of what actually gets written to the file.
          "function cutEditorInitShape(el) {"
          "el.style.fill = 'none';"
          "cutEditorRecolor(el);"
          "var hit = el.cloneNode(false);"
          "hit.setAttribute('data-hit-proxy', '1');"
          "hit.style.fill = 'none';"
          "hit.style.stroke = 'transparent';"
          "hit.style.strokeWidth = '14';"
          "hit.style.pointerEvents = 'stroke';"
          "hit.style.cursor = 'pointer';"
          "hit.addEventListener('click', function(ev) {"
          "ev.stopPropagation();"
          "cutEditorToggleSelect(el, ev.shiftKey);"
          "});"
          "el.parentNode.insertBefore(hit, el.nextSibling);"
          "}"
          "function cutEditorCutTypeOf(el) {"
          "return el.getAttributeNS(EDITOR_SHAPER_NS, 'cutType') || '';"
          "}"
          "function cutEditorRecolor(el) {"
          "var isSelected = cutEditorState.selected.indexOf(el) >= 0;"
          "if (isSelected) {"
          "el.style.stroke = EDITOR_SELECT_COLOR;"
          "el.style.strokeWidth = '4';"
          "el.style.strokeDasharray = '6,3';"
          "} else {"
          "var ct = cutEditorCutTypeOf(el);"
          "el.style.stroke = EDITOR_CUT_COLORS[ct] || EDITOR_UNSET_COLOR;"
          "el.style.strokeWidth = '2';"
          "el.style.strokeDasharray = '';"
          "}"
          "}"
          "function cutEditorToggleSelect(el, additive) {"
          "var idx = cutEditorState.selected.indexOf(el);"
          "if (!additive) {"
          "var wasOnlySelected = (idx >= 0 && cutEditorState.selected.length === 1);"
          "var prev = cutEditorState.selected;"
          "cutEditorState.selected = [];"
          "for (var i = 0; i < prev.length; i++) cutEditorRecolor(prev[i]);"
          "if (!wasOnlySelected) cutEditorState.selected.push(el);"
          "} else if (idx >= 0) {"
          "cutEditorState.selected.splice(idx, 1);"
          "} else {"
          "cutEditorState.selected.push(el);"
          "}"
          "cutEditorRecolor(el);"
          "updateSelectionSummary();"
          "}"
          "function updateSelectionSummary() {"
          "var n = cutEditorState.selected.length;"
          "document.getElementById('cutEditorSelCount').textContent = n === 0 ? 'No line selected.' : (n + ' line(s) selected.');"
          "var applyBtn = document.getElementById('applyToSelectedBtn');"
          "if (applyBtn) applyBtn.disabled = (n === 0);"
          "if (n > 0) {"
          "var el = cutEditorState.selected[0];"
          "document.getElementById('editCutType').value = cutEditorCutTypeOf(el);"
          "var depth = el.getAttributeNS(EDITOR_SHAPER_NS, 'cutDepth') || '';"
          "var depthParts = depth.split(' ');"
          "document.getElementById('editDepth').value = depthParts[0] || '';"
          "if (depthParts[1]) document.getElementById('editDepthUnit').value = depthParts[1];"
          // Pre-fill the tool diameter preset (or the custom row) from
          // this shape's existing shaper:toolDia, the same way depth
          // just did above - this was missing entirely before, so
          // reopening an already-edited shape always showed "choose
          // one" for bit size even though the real value was saved and
          // correct. Matched numerically (parseFloat), not by string,
          // since a written value like "0.125 in" needs to match a
          // preset option written as value='0.125|in'.
          "var toolDia = el.getAttributeNS(EDITOR_SHAPER_NS, 'toolDia') || '';"
          "var toolDiaParts = toolDia.split(' ');"
          "var toolDiaNum = toolDiaParts[0] !== undefined ? parseFloat(toolDiaParts[0]) : NaN;"
          "var toolDiaUnitVal = toolDiaParts[1] || '';"
          "var presetSel = document.getElementById('editToolDiaPreset');"
          "var matchedPreset = '';"
          "if (!isNaN(toolDiaNum) && toolDiaUnitVal) {"
          "for (var pi = 0; pi < presetSel.options.length; pi++) {"
          "var optVal = presetSel.options[pi].value;"
          "if (optVal === '' || optVal === '__custom__') continue;"
          "var optParts = optVal.split('|');"
          "if (parseFloat(optParts[0]) === toolDiaNum && optParts[1] === toolDiaUnitVal) { matchedPreset = optVal; break; }"
          "}"
          "}"
          "if (matchedPreset) {"
          "presetSel.value = matchedPreset;"
          "} else if (!isNaN(toolDiaNum) && toolDiaUnitVal) {"
          "presetSel.value = '__custom__';"
          "document.getElementById('editToolDiaCustom').value = toolDiaParts[0];"
          "document.getElementById('editToolDiaUnit').value = toolDiaUnitVal;"
          "} else {"
          "presetSel.value = '';"
          "document.getElementById('editToolDiaCustom').value = '';"
          "}"
          "handleEditToolDiaPresetChange();"
          "toggleEditToolDiaWrap();"
          "}"
          "}"
          "function toggleEditToolDiaWrap() {"
          "var ct = document.getElementById('editCutType').value;"
          "var needsOffset = (ct === 'outside' || ct === 'inside' || ct === 'pocket');"
          "document.getElementById('editToolDiaWrap').style.display = needsOffset ? '' : 'none';"
          "}"
          "function handleEditToolDiaPresetChange() {"
          "var isCustom = document.getElementById('editToolDiaPreset').value === '__custom__';"
          "document.getElementById('editToolDiaCustomWrap').style.display = isCustom ? '' : 'none';"
          "}"
          "function applyToSelectedShapes() {"
          "var cutType = document.getElementById('editCutType').value;"
          "if (!cutType) { alert('Choose a cut type first.'); return; }"
          "if (cutEditorState.selected.length === 0) { alert('Select at least one line first.'); return; }"
          "var depthVal = document.getElementById('editDepth').value;"
          "var depthUnit = document.getElementById('editDepthUnit').value;"
          "var needsOffset = (cutType === 'outside' || cutType === 'inside' || cutType === 'pocket');"
          "var toolDiaVal = '', toolDiaUnit = 'mm';"
          "if (needsOffset) {"
          "var preset = document.getElementById('editToolDiaPreset').value;"
          "if (preset === '__custom__') {"
          "toolDiaVal = document.getElementById('editToolDiaCustom').value;"
          "toolDiaUnit = document.getElementById('editToolDiaUnit').value;"
          "} else if (preset) {"
          "var presetParts = preset.split('|');"
          "toolDiaVal = presetParts[0];"
          "toolDiaUnit = presetParts[1];"
          "}"
          "if (!toolDiaVal || isNaN(parseFloat(toolDiaVal))) {"
          "alert('Outside/Inside/Pocket cuts need a tool diameter.');"
          "return;"
          "}"
          "}"
          "var depthAttr = null;"
          "if (depthVal !== '' && !isNaN(parseFloat(depthVal))) depthAttr = parseFloat(depthVal) + ' ' + depthUnit;"
          "var toolDiaAttr = needsOffset ? (parseFloat(toolDiaVal) + ' ' + toolDiaUnit) : null;"
          "var sel = cutEditorState.selected.slice();"
          "for (var i = 0; i < sel.length; i++) {"
          "var el = sel[i];"
          "el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:cutType', cutType);"
          "if (depthAttr) el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:cutDepth', depthAttr);"
          "if (toolDiaAttr) {"
          "el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:toolDia', toolDiaAttr);"
          "el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:cutOffset', '0' + toolDiaUnit);"
          "} else {"
          "el.removeAttributeNS(EDITOR_SHAPER_NS, 'toolDia');"
          "el.removeAttributeNS(EDITOR_SHAPER_NS, 'cutOffset');"
          "}"
          "}"
          "cutEditorState.selected = [];"
          "for (var j = 0; j < sel.length; j++) cutEditorRecolor(sel[j]);"
          "cutEditorState.dirty = true;"
          "document.getElementById('cutEditorStatus').textContent = 'Applied to ' + sel.length + ' line(s) - click Save changes to write this to the file.';"
          "updateSelectionSummary();"
          "}"
          "function closeCutEditor() {"
          "if (cutEditorState.dirty && !confirm('Discard unsaved changes?')) return;"
          "document.getElementById('cutEditorOverlay').style.display = 'none';"
          "}"
          "async function saveCutEditor() {"
          "if (!cutEditorState.svgEl) return;"
          "var status = document.getElementById('cutEditorStatus');"
          "status.textContent = 'Saving...';"
          // Serialize a clone of the live svgEl (not svgEl itself, and not
          // the original parsed document - openCutEditor()'s appendChild()
          // already emptied that out, see the comment there), so the
          // hit-testing proxies (see cutEditorInitShape()) never end up
          // written into the actual file, but also so the live editor
          // stays fully functional (proxies intact) if this save attempt
          // fails and the user wants to try again without reopening.
          "var cloneEl = cutEditorState.svgEl.cloneNode(true);"
          "var proxies = cloneEl.querySelectorAll('[data-hit-proxy]');"
          "for (var i = 0; i < proxies.length; i++) proxies[i].parentNode.removeChild(proxies[i]);"
          // Strip every inline style this editor set for its own on-
          // screen preview - the root's responsive width/height/display
          // (see openCutEditor()) and every shape's selection-highlight/
          // cut-type-legend coloring (see cutEditorRecolor()) - none of
          // which should ever end up in the saved file. None of it is
          // needed there: the shaper:* cut-type data lives in its own
          // namespaced attributes, set separately and untouched by this.
          "cloneEl.removeAttribute('style');"
          "var styledEls = cloneEl.querySelectorAll('[style]');"
          "for (var s = 0; s < styledEls.length; s++) styledEls[s].removeAttribute('style');"
          "var svgText = new XMLSerializer().serializeToString(cloneEl);"
          "var fd = new FormData();"
          "fd.append('file', new Blob([svgText], {type: 'image/svg+xml'}), cutEditorState.name);"
          "try {"
          "var resp = await fetch('/upload?folder=' + encodeURIComponent(cutEditorState.folder), {method: 'POST', body: fd});"
          "if (resp.ok) {"
          "status.textContent = 'Saved. Reloading...';"
          "cutEditorState.dirty = false;"
          "setTimeout(function() { location.reload(); }, 700);"
          "} else {"
          "status.textContent = 'Save failed (HTTP ' + resp.status + ').';"
          "}"
          "} catch (err) {"
          "status.textContent = 'Save failed (' + err.message + ').';"
          "}"
          "}"
          "document.getElementById('uploadBtn').addEventListener('click', async function() {"
          "var fileInput = document.getElementById('uploadFile');"
          "var status = document.getElementById('uploadStatus');"
          "if (!fileInput.files.length) { status.textContent = 'Choose a file first.'; return; }"
          "var files = Array.prototype.slice.call(fileInput.files);"
          "var unit = document.getElementById('dxfUnit').value;"
          "var uploadFolder = document.getElementById('uploadFolder').value;"
          "if (uploadFolder === '__new__') uploadFolder = '';"
          "var cutType = document.getElementById('cutType').value;"
          "var cutDepthVal = document.getElementById('cutDepth').value;"
          "var cutDepthUnit = document.getElementById('cutDepthUnit').value;"
          "var toolDiaPresetVal = document.getElementById('toolDiaPreset').value;"
          "var toolDiaVal, toolDiaUnit;"
          "if (toolDiaPresetVal === '__custom__') {"
          "toolDiaVal = document.getElementById('toolDiaCustom').value;"
          "toolDiaUnit = document.getElementById('toolDiaUnit').value;"
          "} else if (toolDiaPresetVal) {"
          "var toolDiaParts = toolDiaPresetVal.split('|');"
          "toolDiaVal = toolDiaParts[0];"
          "toolDiaUnit = toolDiaParts[1];"
          "} else {"
          "toolDiaVal = '';"
          "toolDiaUnit = 'mm';"
          "}"
          // Pass 1: convert every selected DXF client-side first (SVGs pass
          // through untouched). This means we know every output filename
          // before asking about overwrites, so the user gets one prompt
          // covering the whole batch instead of one popup per file.
          "status.textContent = 'Preparing ' + files.length + ' file(s)...';"
          "var jobs = [];"
          "for (var i = 0; i < files.length; i++) {"
          "var f = files[i];"
          "try {"
          "if (/\\.dxf$/i.test(f.name)) {"
          "var text = await f.text();"
          "var result = RouterDriveDXF.toSVG(text, {outputUnit: unit});"
          "var msg = result.widthMM.toFixed(1) + ' x ' + result.heightMM.toFixed(1) + ' mm, ' + result.pathCount + ' path(s)';"
          "var skippedParts = [];"
          "for (var k in result.skipped) skippedParts.push(result.skipped[k] + ' ' + k);"
          "if (skippedParts.length) msg += ', skipped ' + skippedParts.join(', ');"
          "var svgName = f.name.replace(/\\.dxf$/i, '') + '.svg';"
          "var svgOut = result.svg;"
          "if (cutType || cutDepthVal) svgOut = applyShaperMetadata(svgOut, cutType, cutDepthVal, cutDepthUnit, toolDiaVal, toolDiaUnit);"
          "jobs.push({name: f.name, svgName: svgName, blob: new Blob([svgOut], {type: 'image/svg+xml'}), msg: msg});"
          "} else {"
          "if (cutType || cutDepthVal) {"
          "var svgText = await f.text();"
          "svgText = applyShaperMetadata(svgText, cutType, cutDepthVal, cutDepthUnit, toolDiaVal, toolDiaUnit);"
          "jobs.push({name: f.name, svgName: f.name, blob: new Blob([svgText], {type: 'image/svg+xml'})});"
          "} else {"
          "jobs.push({name: f.name, svgName: f.name, blob: f});"
          "}"
          "}"
          "} catch (err) {"
          "jobs.push({name: f.name, error: err.message});"
          "}"
          "}"
          "var toUpload = jobs.filter(function(j) { return !j.error; });"
          "var failed = jobs.filter(function(j) { return j.error; }).map(function(j) { return j.name + ' (' + j.error + ')'; });"
          // A DXF and an SVG in the same batch can land on the same output
          // name (e.g. "part.dxf" converts to "part.svg", and you also
          // picked a literal "part.svg") - rather than one silently
          // clobbering the other on the device, auto-rename every name
          // after the first occurrence to "..._1.svg", "..._2.svg", etc.
          // Only applies within this batch; a name that collides with a
          // file already on the drive still goes through the overwrite
          // confirmation below, same as ever.
          "var nameCounts = {};"
          "for (var i = 0; i < toUpload.length; i++) {"
          "var original = toUpload[i].svgName;"
          "var count = nameCounts[original] || 0;"
          "if (count > 0) {"
          "var dot = original.lastIndexOf('.');"
          "var base = dot === -1 ? original : original.slice(0, dot);"
          "var ext = dot === -1 ? '' : original.slice(dot);"
          "toUpload[i].svgName = base + '_' + count + ext;"
          "}"
          "nameCounts[original] = count + 1;"
          "}"
          "var conflicts = findOverwriteConflicts(toUpload.map(function(j) { return j.svgName; }), uploadFolder);"
          "if (conflicts.length && !confirm('This will overwrite: ' + conflicts.join(', ') + '. Continue?')) {"
          "status.textContent = 'Upload cancelled.';"
          "return;"
          "}"
          // Pass 2: upload one at a time (not concurrently) - each upload
          // briefly switches the device out of USB mode to write the file,
          // so overlapping requests could race that switch.
          "var uploaded = [];"
          "for (var i = 0; i < toUpload.length; i++) {"
          "var j = toUpload[i];"
          "status.textContent = 'Uploading ' + (i + 1) + ' of ' + toUpload.length + ': ' + j.svgName + '...';"
          "try {"
          "var fd = new FormData();"
          "fd.append('file', j.blob, j.svgName);"
          "var resp = await fetch('/upload?folder=' + encodeURIComponent(uploadFolder), {method: 'POST', body: fd});"
          "if (resp.ok) { uploaded.push(j.svgName); }"
          "else { failed.push(j.svgName + ' (HTTP ' + resp.status + ')'); }"
          "} catch (err) {"
          "failed.push(j.svgName + ' (' + err.message + ')');"
          "}"
          "}"
          "var summary = uploaded.length ? ('Uploaded ' + uploaded.length + ' file(s): ' + uploaded.join(', ') + '.') : 'Nothing uploaded.';"
          "if (failed.length) summary += ' Failed: ' + failed.join(', ') + '.';"
          "status.textContent = summary;"
          "if (uploaded.length) { status.textContent += ' Reloading...'; setTimeout(function(){ location.reload(); }, 900); }"
          "});"
          "</script>";
  html += "</div>"; // .page

  html += "<footer>";
  html += renderBuildFooter();
  html += renderLedToggle();
  html += "</footer>";
  html += "</body></html>";
  return html;
}

// ---------------------------------------------------------------------------
// route handlers
// ---------------------------------------------------------------------------

static void handleIndex() {
  if (wifiPortalState() == WIFI_STATE_AP && !apFullPageUnlocked) {
    server.send(200, "text/html", renderCaptivePage());
    return;
  }
  server.send(200, "text/html", renderPage());
}

// GET /nowifi - the "Prefer to skip this?" escape hatch from the AP-mode
// streamlined page (see renderCaptivePage()). Sets apFullPageUnlocked so
// every subsequent "/" request shows the full page instead, for the rest
// of this boot (there's no per-client tracking - this is a single-user
// device, and the flag naturally resets to false on every restart, which
// is also the only way to get a fresh AP-mode session in the first
// place). Meant to be opened directly in a real browser tab, not clicked
// from inside the captive-window popup - see the README for why.
static void handleNoWifi() {
  apFullPageUnlocked = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

// Fires once per file part in the request body - a plain single-file
// upload has exactly one, but the "Upload files" input now allows
// selecting several at once (and each converted DXF gets uploaded as its
// own request too), which the browser sends as one part per file (all
// still named "file") within the same multipart POST. The WebServer
// library walks those parts one at a time, each running its own
// START -> WRITE(s) -> END, so this just needs to treat every START as a
// fresh file rather than assuming there's only ever one.
static void handleUploadData() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    ledSet(LED_BLINK_FAST); // busy - harmless to call again for each part in a multi-file batch
    String base = basenameOf(upload.filename);
    if (base.length() == 0) {
      base = "upload.svg";
    }
    if (!storageBeginAppAccess()) {
      uploadBatch.failed.push_back(base);
      return;
    }
    // "folder" travels as a URL query param (see the upload JS) rather
    // than a multipart field, since it's the same for every file in the
    // batch and query args are readable here regardless of the POST
    // body's content type. Re-validated server-side - never trust the
    // client blindly, even though the UI only ever offers known-good
    // names.
    String folder = server.hasArg("folder") ? server.arg("folder") : "";
    if (folder.length() > 0 && !isValidFolderName(folder)) {
      folder = "";
    }
    if (folder.length() > 0 && !FFat.exists(folderDirPath(folder))) {
      FFat.mkdir(folderDirPath(folder));
    }
    String path = joinFolder(folder, base);
    // Overwriting an existing file (re-uploading the same name - which is
    // exactly what saving in the per-line cut editor does every time, and
    // what re-uploading a same-named DXF/SVG from the Upload section does
    // too) must start from a genuinely empty file. FFat.open(path,
    // FILE_WRITE) was relied on to truncate any existing file at that
    // path, but real-hardware testing of the cut editor showed saved
    // files coming out corrupted - missing their opening <svg> tag,
    // missing the xmlns:shaper declaration, containing stray null bytes,
    // and containing what looked like leftover content from a PREVIOUS
    // save of the same file mixed in with the new one (mismatched
    // shaper:cutType values for the same path between two halves of one
    // saved file) - i.e., new writes were landing on top of old bytes
    // rather than starting clean, consistent with the open-for-write not
    // truncating on this filesystem. Removing the old file outright
    // before opening for write sidesteps that regardless of the exact
    // underlying cause, guaranteeing every save starts from zero bytes.
    if (FFat.exists(path)) {
      FFat.remove(path);
    }
    uploadFile = FFat.open(path, FILE_WRITE);
    if (!uploadFile) {
      uploadBatch.failed.push_back(base);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    String base = basenameOf(upload.filename);
    if (base.length() == 0) {
      base = "upload.svg";
    }
    if (uploadFile) {
      uploadFile.close();
      uploadBatch.uploaded.push_back(base);
    }
    // else: this part already landed in uploadBatch.failed back at START.
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
    }
    uploadBatch.failed.push_back(basenameOf(upload.filename));
  }
}

static void handleUploadDone() {
  if (storageInAppMode()) {
    storageEndAppAccess(true); // rescan so the Origin notices the new file(s)
  }

  size_t okCount = uploadBatch.uploaded.size();
  size_t failCount = uploadBatch.failed.size();
  if (okCount == 0 && failCount == 0) {
    flashMessage = ""; // form submitted with nothing selected, nothing to say
  } else if (failCount == 0) {
    flashMessage = okCount == 1 ? ("Uploaded " + uploadBatch.uploaded[0])
                                 : ("Uploaded " + String(okCount) + " files");
    flashIsError = false;
  } else if (okCount == 0) {
    flashMessage = "Upload failed: " + joinNames(uploadBatch.failed);
    flashIsError = true;
  } else {
    flashMessage = "Uploaded " + String(okCount) + " file(s), failed: " + joinNames(uploadBatch.failed);
    flashIsError = true;
  }
  for (size_t i = 0; i < uploadBatch.uploaded.size(); i++) {
    justUploadedNames.push_back(uploadBatch.uploaded[i]);
  }
  uploadBatch.uploaded.clear();
  uploadBatch.failed.clear();
  ledApplyIdleState();

  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  if (folder.length() > 0 && !isValidFolderName(folder)) {
    folder = "";
  }
  server.sendHeader("Location", folder.length() > 0 ? ("/?dir=" + urlEncode(folder)) : "/");
  server.send(303);
}

// The delete form now submits one checkbox per selected file, all named
// "name" - loop over every arg with that name instead of assuming exactly
// one, so a single POST can remove any number of files at once.
static void handleDelete() {
  String folder = server.hasArg("dir") ? server.arg("dir") : "";
  if (folder.length() > 0 && !isValidFolderName(folder)) {
    folder = "";
  }
  String redirectTo = folder.length() > 0 ? ("/?dir=" + urlEncode(folder)) : "/";

  std::vector<String> names;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "name") {
      String base = basenameOf(server.arg(i));
      if (base.length() > 0) names.push_back(base);
    }
  }
  if (names.empty()) {
    server.sendHeader("Location", redirectTo);
    server.send(303);
    return;
  }

  ledSet(LED_BLINK_FAST); // busy

  std::vector<String> deleted;
  std::vector<String> failed;
  if (storageBeginAppAccess()) {
    for (size_t i = 0; i < names.size(); i++) {
      if (FFat.remove(joinFolder(folder, names[i]))) {
        deleted.push_back(names[i]);
      } else {
        failed.push_back(names[i]);
      }
    }
    storageEndAppAccess(true);
  } else {
    failed = names;
  }

  if (failed.empty()) {
    flashMessage = deleted.size() == 1 ? ("Deleted " + deleted[0])
                                        : ("Deleted " + String(deleted.size()) + " files");
    flashIsError = false;
  } else if (deleted.empty()) {
    flashMessage = "Could not delete: " + joinNames(failed);
    flashIsError = true;
  } else {
    flashMessage = "Deleted " + String(deleted.size()) + " file(s), failed: " + joinNames(failed);
    flashIsError = true;
  }
  ledApplyIdleState();
  server.sendHeader("Location", redirectTo);
  server.send(303);
}

// Creates an empty folder on demand - used by the Files section's
// "+ New folder..." dropdown entry, which (unlike the Upload section's
// same-named entry) has no upload to piggyback the mkdir onto, since
// browsing there doesn't necessarily involve uploading anything.
static void handleMkdir() {
  String name = server.hasArg("name") ? server.arg("name") : "";
  name.trim();
  String redirectTo = "/";
  if (name.length() == 0 || !isValidFolderName(name)) {
    flashMessage = "Invalid folder name";
    flashIsError = true;
    server.sendHeader("Location", redirectTo);
    server.send(303);
    return;
  }
  if (!storageBeginAppAccess()) {
    flashMessage = "Could not access storage";
    flashIsError = true;
    server.sendHeader("Location", redirectTo);
    server.send(303);
    return;
  }
  String path = folderDirPath(name);
  bool alreadyExists = FFat.exists(path);
  bool ok = alreadyExists || FFat.mkdir(path);
  storageEndAppAccess(!alreadyExists && ok); // only nudge the host if we actually changed something
  if (ok) {
    // Land the browser back on the folder that was just created, same as
    // the Upload section does after an upload that created one.
    redirectTo = "/?dir=" + urlEncode(name);
  } else {
    flashMessage = "Could not create folder";
    flashIsError = true;
  }
  server.sendHeader("Location", redirectTo);
  server.send(303);
}

// Deletes a folder and every file directly inside it (folders are one
// level deep only, so there's never a nested folder to worry about, bar
// stray OS junk - see the ".Trashes" filter in renderFilesSection() -
// which would just make FFat.rmdir() fail below, a safe failure mode).
static void handleDeleteFolder() {
  String folder = server.hasArg("dir") ? server.arg("dir") : "";
  folder.trim();
  if (folder.length() == 0 || !isValidFolderName(folder)) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  ledSet(LED_BLINK_FAST); // busy

  bool ok = false;
  if (storageBeginAppAccess()) {
    String path = folderDirPath(folder);
    File dir = FFat.open(path);
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        bool isDir = f.isDirectory();
        String childName = basenameOf(String(f.name()));
        f.close();
        if (!isDir) {
          FFat.remove(joinFolder(folder, childName));
        }
        f = dir.openNextFile();
      }
    }
    dir.close(); // must close before either rmdir() or storageEndAppAccess() unmounts
    ok = FFat.rmdir(path);
    storageEndAppAccess(true);
  }

  flashMessage = ok ? ("Deleted folder " + folder) : ("Could not delete folder " + folder);
  flashIsError = !ok;
  ledApplyIdleState();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Moves any number of checked files from one folder to another via
// FFat.rename() (a real rename/move on the underlying FAT filesystem,
// not a copy+delete) - see the Files section's "Move selected" button.
static void handleMove() {
  String srcFolder = server.hasArg("dir") ? server.arg("dir") : "";
  if (srcFolder.length() > 0 && !isValidFolderName(srcFolder)) {
    srcFolder = "";
  }
  String destFolder = server.hasArg("dest") ? server.arg("dest") : "";
  if (destFolder.length() > 0 && !isValidFolderName(destFolder)) {
    destFolder = "";
  }
  String redirectTo = srcFolder.length() > 0 ? ("/?dir=" + urlEncode(srcFolder)) : "/";

  std::vector<String> names;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "name") {
      String base = basenameOf(server.arg(i));
      if (base.length() > 0) names.push_back(base);
    }
  }
  if (names.empty() || srcFolder == destFolder) {
    server.sendHeader("Location", redirectTo);
    server.send(303);
    return;
  }

  ledSet(LED_BLINK_FAST); // busy

  std::vector<String> moved;
  std::vector<String> failed;
  if (storageBeginAppAccess()) {
    if (destFolder.length() > 0 && !FFat.exists(folderDirPath(destFolder))) {
      FFat.mkdir(folderDirPath(destFolder));
    }
    for (size_t i = 0; i < names.size(); i++) {
      String from = joinFolder(srcFolder, names[i]);
      String to = joinFolder(destFolder, names[i]);
      if (FFat.exists(to)) {
        failed.push_back(names[i] + " (already exists there)");
      } else if (FFat.rename(from, to)) {
        moved.push_back(names[i]);
      } else {
        failed.push_back(names[i]);
      }
    }
    storageEndAppAccess(true);
  } else {
    failed = names;
  }

  if (failed.empty()) {
    flashMessage = moved.size() == 1 ? ("Moved " + moved[0])
                                      : ("Moved " + String(moved.size()) + " files");
    flashIsError = false;
  } else if (moved.empty()) {
    flashMessage = "Could not move: " + joinNames(failed);
    flashIsError = true;
  } else {
    flashMessage = "Moved " + String(moved.size()) + " file(s), failed: " + joinNames(failed);
    flashIsError = true;
  }
  ledApplyIdleState();
  server.sendHeader("Location", redirectTo);
  server.send(303);
}

// The old "soft" rescan (toggling mediaPresent() without restarting, via
// usbSoftRescan()) used to live behind this same route/button as a quick
// no-Wi-Fi-drop alternative. Removed: real testing showed it never once
// got the Origin to notice a new file, and clicking it repeatedly (4x in a
// row) once made the Origin appear to lose the drive entirely instead -
// see README/project notes. Restart is slower but is what's actually been
// reliable, so it's the only option left here now.
static void handleRescan() {
  // usbHardReconnect() restarts the whole device - respond to the browser
  // FIRST, or it'll just hang waiting for a reply that never comes. This is
  // a normal HTML form POST (no JS on the button itself), so without help
  // the browser's address bar would just sit on "/rescan" showing whatever
  // this response was, forever - it has no way to know the device is even
  // coming back, let alone when. So this response includes a small script
  // that polls "/" every second until the device answers again, then
  // navigates there itself.
  server.send(200, "text/html",
              "<html><body><p>Restarting device to force a USB reconnect - "
              "this takes a few seconds and Wi-Fi will drop too.</p>"
              "<p id='rescanMsg'>Waiting for it to come back online...</p>"
              "<script>"
              "var attempts = 0;"
              "function tryReload() {"
              "attempts++;"
              "fetch('/', {cache: 'no-store'}).then(function(r) {"
              "if (r.ok) { location.href = '/'; return; }"
              "scheduleRetry();"
              "}).catch(scheduleRetry);"
              "}"
              "function scheduleRetry() {"
              "if (attempts > 40) {"
              "document.getElementById('rescanMsg').textContent = "
              "'Still not reachable after 40+ seconds - try refreshing manually, or "
              "revisit http://" + String(HOSTNAME) + ".local/';"
              "return;"
              "}"
              "setTimeout(tryReload, 1000);"
              "}"
              "setTimeout(tryReload, 3000);"
              "</script>"
              "</body></html>");
  ledFlashConfirm(); // visible "yes, this registered" moment - also serves as the pre-restart delay
  usbHardReconnect(); // does not return
}

static void handleLedToggle() {
  ledSetEnabled(!ledIsEnabled());
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() == 0) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  server.send(200, "text/html",
              "<html><body><p>Saved. Restarting and attempting to join \"" + htmlEscape(ssid) +
              "\"...</p><p>If it can't connect within a few seconds it will fall back to the "
              "setup network (\"" + String(PORTAL_SSID) + "\") again.</p></body></html>");
  delay(200);
  wifiPortalSaveCredentials(ssid, password); // reboots
}

static void handleWifiReset() {
  server.send(200, "text/html", "<html><body><p>Wi-Fi settings cleared. Restarting...</p></body></html>");
  delay(200);
  wifiPortalResetCredentials(); // reboots
}

static void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) {
      json += ",";
    }
    json += "\"" + WiFi.SSID(i) + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

static void handleDxfScript() {
  // DXF2SVG_JS_SOURCE lives in flash (PROGMEM); send_P streams it straight
  // from there instead of copying the whole ~16KB into a String first.
  server.send_P(200, "application/javascript", DXF2SVG_JS_SOURCE);
}

static void handleStyleCss() {
  server.send_P(200, "text/css", STYLE_CSS_SOURCE);
}

// Serves a stored file's full raw SVG content so the file list's per-line
// cut editor (see the Cut type column in renderFilesSection()) can fetch
// and render it client-side. Unlike readShaperInfo()'s chunked stream
// (which runs once per file while building an entire folder listing -
// many files at once, so it's deliberately bounded to a small buffer),
// this reads a file's whole content into one allocation - acceptable
// here because it only ever runs for one file at a time, on demand, when
// a user explicitly opens that file's editor, not once per row in a
// listing. A pathologically large SVG could still fail the single heap
// allocation below on this board's limited SRAM; worth watching for on
// hardware with very complex/dense geometry (same caveat class as
// readShaperInfo()'s SHAPER_SCAN_HARD_CAP above).
static void handleGetSvg() {
  String name = server.hasArg("name") ? basenameOf(server.arg("name")) : "";
  String folder = server.hasArg("dir") ? server.arg("dir") : "";
  if (folder.length() > 0 && !isValidFolderName(folder)) folder = "";
  if (name.length() == 0) {
    server.send(400, "text/plain", "Missing name");
    return;
  }
  if (!storageBeginAppAccess()) {
    server.send(503, "text/plain", "Could not access storage");
    return;
  }
  File f = FFat.open(joinFolder(folder, name), FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    storageEndAppAccess(false);
    server.send(404, "text/plain", "Not found");
    return;
  }
  size_t sz = f.size();
  char *buf = new char[sz + 1];
  size_t n = f.read((uint8_t *)buf, sz);
  buf[n] = '\0';
  String content(buf);
  delete[] buf;
  f.close();
  storageEndAppAccess(false); // just reading, nothing changed - no need to nudge the host
  server.send(200, "image/svg+xml", content);
}

static void handleNotFound() {
  // Captive-portal-friendly: send everything unknown back to "/" so phones
  // and laptops pop the "sign in to network" prompt during AP/setup mode.
  server.sendHeader("Location", "/");
  server.send(302);
}

void webServerInit() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/nowifi", HTTP_GET, handleNoWifi);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/mkdir", HTTP_POST, handleMkdir);
  server.on("/deletefolder", HTTP_POST, handleDeleteFolder);
  server.on("/move", HTTP_POST, handleMove);
  server.on("/rescan", HTTP_POST, handleRescan);
  server.on("/led-toggle", HTTP_POST, handleLedToggle);
  server.on("/dxf2svg.js", HTTP_GET, handleDxfScript);
  server.on("/style.css", HTTP_GET, handleStyleCss);
  server.on("/svg", HTTP_GET, handleGetSvg);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/wifi-reset", HTTP_POST, handleWifiReset);
  server.on("/scan", HTTP_GET, handleScan);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[web] server started on port 80");
}

void webServerLoop() {
  server.handleClient();
}
