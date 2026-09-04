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

static const int MAX_FILE_NAME_LEN = 64;

// File names the user typed into the Rename dialog. Looser than
// isValidFolderName (a "." has to be allowed, or you couldn't keep the
// extension) but still deliberately boring - nothing that could climb out
// of its folder or upset FAT. Note this runs on an already-basenameOf()'d
// string, so a slash can only appear here if something went wrong, and it
// is rejected rather than quietly stripped.
static bool isValidFileName(const String &name) {
  if (name.length() == 0 || name.length() > MAX_FILE_NAME_LEN) return false;
  if (name == "." || name == "..") return false;
  if (name[0] == ' ' || name[name.length() - 1] == ' ') return false;
  if (name[0] == '.') return false; // no hidden/extension-only names
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static String extensionOf(const String &name) {
  int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(dot) : String("");
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
};

// Every shape this app writes shaper:* attributes onto used to carry the
// same cutType, back when applyShaperMetadata() only ever applied
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
// "-" for the cut type (a Bit size column existed then too, and showed
// the same), even though the shapes'
// attributes were saved correctly and the editor itself displayed them
// fine on reopen (it reads the whole file via GET /svg, no size cap) -
// their attributes simply landed past the old 4KB window. Fixed below by
// streaming the file through a small fixed-size chunk buffer instead of
// a size-proportional one, so correctness no longer depends on file size
// while peak memory use is still bounded and, in fact, smaller than the
// old worst case (one ~2KB buffer instead of up to 4KB).
static const size_t SHAPER_SCAN_CHUNK = 2048;
// Every '<attr>="<value>"' this app ever writes is well under this many
// bytes (the longest, shaper:cutDepth="0.125 in", is ~26) - carrying
// this many bytes from the end of one chunk into the front of the next
// guarantees no occurrence is ever split across a chunk boundary, so
// each chunk can still be scanned independently.
static const size_t SHAPER_SCAN_OVERLAP = 64;
// Sane ceiling on total bytes scanned per file, purely so one huge file
// can't make the whole folder listing crawl - well beyond any realistic
// converted SVG for this device's use case (a Shaper Studio export of a
// real multi-shape design was ~19 KB).
static const size_t SHAPER_SCAN_HARD_CAP = 262144;

// ---------------------------------------------------------------------------
// Cut type detection from COLOR, for files this app didn't write.
//
// Origin selects a shape's cut type from its fill/stroke color, not from
// shaper:cutType (see applyShaperCutColors() in the page script for the
// full story). So a file prepared in Affinity, Illustrator or anything
// else that follows Shaper's published color encoding is perfectly valid
// and will cut correctly - it just carries no shaper:* attributes at all,
// which used to leave the file list showing nothing for it. This scans
// for the colors themselves so those files report their real cut types.
//
// Deliberately NOT per-element: associating a fill with its own shape
// would mean parsing whole <path> tags, which can be many KB each (the d
// attribute) and would blow past the small chunk buffer this streams
// through. It doesn't need to - the encoding is unambiguous per color
// (fill-encoded types always pair with no stroke, stroke-encoded types
// with no fill), so "which cut types appear anywhere in this file" is
// answerable from the set of colors present, which is exactly what the
// Cut type column reports.
// ---------------------------------------------------------------------------
static const uint8_t CUTCOLOR_OUTSIDE = 1 << 0; // black fill
static const uint8_t CUTCOLOR_INSIDE  = 1 << 1; // white fill
static const uint8_t CUTCOLOR_POCKET  = 1 << 2; // gray fill
static const uint8_t CUTCOLOR_ONLINE  = 1 << 3; // gray stroke
static const uint8_t CUTCOLOR_GUIDE   = 1 << 4; // blue stroke

// Reads a color literal straight out of the buffer it was found in, with
// no String and no allocation: parses #abc, #aabbcc and rgb(r, g, b) into
// channel values, or returns false for anything not comparable (none,
// currentColor, a named color, a gradient url). Liberal about spelling on
// purpose - this is the code path that exists to read OTHER tools' files.
//
// This whole scanner used to work in Arduino Strings, which meant an
// allocation per chunk, per match and per parse step. Measured on a real
// Shaper Studio export that was 40 allocations and 57KB of heap traffic
// for one file; on a 100KB drawing, 200 allocations and 305KB. All of it
// short-lived and odd-sized, which is the shape that fragments a heap
// this small. It is now zero on both counts, and the benchmark that
// established those numbers also asserts both versions return the same
// answer for every input.
static bool parseColorAt(const char *s, size_t len, int &r, int &g, int &b) {
  while (len && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; len--; }
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
  if (len == 0) return false;
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
  };
  if (s[0] == '#') {
    if (len == 4) { // #abc shorthand
      int h[3];
      for (int i = 0; i < 3; i++) { h[i] = hexVal(s[1 + i]); if (h[i] < 0) return false; }
      r = h[0] * 17; g = h[1] * 17; b = h[2] * 17; // 0xa -> 0xaa
      return true;
    }
    if (len == 7) {
      int h[6];
      for (int i = 0; i < 6; i++) { h[i] = hexVal(s[1 + i]); if (h[i] < 0) return false; }
      r = h[0] * 16 + h[1]; g = h[2] * 16 + h[3]; b = h[4] * 16 + h[5];
      return true;
    }
    return false;
  }
  if (len > 4 && strncasecmp(s, "rgb(", 4) == 0) {
    int vals[3] = {0, 0, 0};
    int idx = 0;
    size_t i = 4;
    while (i < len && idx < 3) {
      while (i < len && (s[i] == ' ' || s[i] == ',')) i++;
      if (i >= len || s[i] < '0' || s[i] > '9') break;
      int v = 0;
      while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
      if (v > 255) return false;
      vals[idx++] = v;
    }
    if (idx < 3) return false;
    r = vals[0]; g = vals[1]; b = vals[2];
    return true;
  }
  return false;
}

// Origin matches these tolerantly rather than by exact hex - Shaper's own
// "readable gray" guide just tells you to make R, G and B equal - so
// match on the same basis instead of demanding the exact values Shaper's
// software happens to emit.
static uint8_t classifyFill(int r, int g, int b) {
  if (r == g && g == b) {
    if (r <= 32) return CUTCOLOR_OUTSIDE;  // black fill
    if (r >= 224) return CUTCOLOR_INSIDE;  // white fill
    return CUTCOLOR_POCKET;                 // any other true gray
  }
  return 0;
}

static uint8_t classifyStroke(int r, int g, int b) {
  // Blue guide lines: clearly blue-dominant rather than an exact #0068FF.
  if (b >= 128 && b > r + 60 && b > g + 60) return CUTCOLOR_GUIDE;
  // Gray stroke = On Line. Pure black is excluded on purpose: it's the
  // companion stroke of a white-filled Interior cut, not a type of its
  // own, and it's also what a generic un-encoded outline SVG uses.
  if (r == g && g == b && r > 32 && r < 224) return CUTCOLOR_ONLINE;
  return 0;
}

// Collects every fill/stroke color in one NUL-terminated buffer, in both
// the presentation attribute form (fill="#000") and the inline style form
// (style="fill:#000;stroke:none") that other design tools commonly emit.
// The needles include the delimiter, so stroke-width="..." and
// fill-rule:... can't be mistaken for a color.
static void scanBufferColors(const char *buf, size_t len, uint8_t &hits) {
  struct { const char *needle; size_t nlen; char terminator; bool isFill; } probes[] = {
    {"fill=\"",   6, '"', true},
    {"stroke=\"", 8, '"', false},
    {"fill:",     5, ';', true},
    {"stroke:",   7, ';', false},
  };
  const char *endBuf = buf + len;
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
    const char *cur = buf;
    while (true) {
      const char *hit = strstr(cur, probes[p].needle);
      if (!hit || hit >= endBuf) break;
      const char *start = hit + probes[p].nlen;
      // A style-form value can end at ';' OR at the closing quote of the
      // style attribute itself, whichever comes first.
      const char *stop = (const char *)memchr(start, probes[p].terminator, endBuf - start);
      if (probes[p].terminator == ';') {
        const char *quote = (const char *)memchr(start, '"', endBuf - start);
        if (quote && (!stop || quote < stop)) stop = quote;
      }
      if (!stop) break;
      int r, g, b;
      if (parseColorAt(start, (size_t)(stop - start), r, g, b)) {
        hits |= probes[p].isFill ? classifyFill(r, g, b) : classifyStroke(r, g, b);
      }
      cur = stop;
    }
  }
}

static int countBits(uint8_t v) {
  int n = 0;
  while (v) { n += (v & 1); v >>= 1; }
  return n;
}

// Maps a shaper:cutType value onto the same bit the color scanner uses,
// so the file's two ways of stating a cut type land in one shared set
// instead of one overriding the other. An unrecognized value contributes
// nothing, exactly like an unrecognized color. Compared in place against
// the buffer rather than through a String, same as the colors above.
static uint8_t cutTypeBitAt(const char *v, size_t len) {
  if (len == 7 && memcmp(v, "outside", 7) == 0) return CUTCOLOR_OUTSIDE;
  if (len == 6 && memcmp(v, "inside", 6) == 0) return CUTCOLOR_INSIDE;
  if (len == 6 && memcmp(v, "pocket", 6) == 0) return CUTCOLOR_POCKET;
  if (len == 6 && memcmp(v, "online", 6) == 0) return CUTCOLOR_ONLINE;
  if (len == 5 && memcmp(v, "guide", 5) == 0) return CUTCOLOR_GUIDE;
  return 0;
}

// Collects every shaper:cutType in one buffer into that shared set.
// Buffers overlap by SHAPER_SCAN_OVERLAP bytes, so an occurrence sitting
// near a boundary can be counted twice - harmless here, since OR-ing a
// bit that is already set changes nothing. That is a real simplification
// over what this replaced, which tracked "first value seen" plus a mixed
// flag across chunks and had to be careful about exactly that overlap.
static void scanBufferForCutTypeAttr(const char *buf, size_t len, uint8_t &bits) {
  const char *cur = buf;
  const char *endBuf = buf + len;
  while (true) {
    const char *hit = strstr(cur, "shaper:cutType=\"");
    if (!hit || hit >= endBuf) break;
    const char *start = hit + 16; // strlen("shaper:cutType=\"")
    const char *stop = (const char *)memchr(start, '"', endBuf - start);
    if (!stop) break;
    bits |= cutTypeBitAt(start, (size_t)(stop - start));
    cur = stop + 1;
  }
}

// Streams an already-open file handle (left at its current read position
// - call this before any other read on the same handle, and before the
// handle is closed) through a small fixed-size buffer, collecting every
// cut type the file states - both by shaper:cutType attribute and by the
// fill/stroke colors Origin itself reads - into one set. cutType comes
// back as the sentinel raw value "mixed" (see cutTypeLabel()) when that
// set holds more than one, and blank (shown as "Unset") when the file
// states none: no shaper:* attributes AND no color Origin recognizes.
//
// The two sources are unioned rather than ranked, and that is load
// bearing. An earlier version let shaper:cutType win outright and only
// consulted colors for files carrying no attributes at all, which broke
// the most ordinary editing workflow there is: convert a DXF (every path
// gray, so On Line), then set one shape to Outside in the per-line
// editor. The edited shape is the only one with an attribute, so the
// attribute scan saw a single value, called the file Outside, and threw
// the colors away - hiding the several On Line paths sitting right next
// to it. Reported from real use, reproduced in test_color_detect.cpp.
static void readShaperInfo(File &f, String &cutType) {
  cutType = "";
  if (f.size() == 0) return;
  uint8_t typeBits = 0;
  // One buffer, allocated once and reused: the carried-over overlap sits
  // at the front and each fresh read lands directly behind it, so there
  // is no concatenation and no per-chunk String. (The previous version
  // built `overlapTail + String(buf)` every pass, which is where most of
  // that 40-to-200 allocations per file came from.)
  char *buf = new char[SHAPER_SCAN_OVERLAP + SHAPER_SCAN_CHUNK + 1];
  size_t tail = 0;
  size_t totalRead = 0;
  while (totalRead < SHAPER_SCAN_HARD_CAP) {
    size_t n = f.read((uint8_t *)buf + tail, SHAPER_SCAN_CHUNK);
    if (n == 0) break; // end of file
    size_t len = tail + n;
    buf[len] = '\0'; // the scanners use strstr, so termination is required
    totalRead += n;
    scanBufferForCutTypeAttr(buf, len, typeBits);
    scanBufferColors(buf, len, typeBits);
    // No early break once "mixed" is known: both scans have to see the
    // whole file, and stopping early would make the answer depend on
    // where in the file a given shape happened to sit.
    tail = len > SHAPER_SCAN_OVERLAP ? SHAPER_SCAN_OVERLAP : len;
    memmove(buf, buf + len - tail, tail);
  }
  delete[] buf;

  // A gray stroke reads as On Line - including on a file this app's own
  // DXF converter produced with cut type left on "Default". That file
  // really is encoded as On Line, and the Origin really will cut on the
  // line, so reporting "Unset" would be describing the uploader's intent
  // rather than the file. This column answers "what will the machine do
  // with this?", and an earlier version got that wrong in both
  // directions: it also hid a third-party file whose author had
  // deliberately set every line to On Line in Affinity or Inkscape.
  //
  // "Unset" keeps its meaning: a file with no recognized encoding at all
  // - a plain black outline from a generic drawing tool, say - still has
  // nothing here for Origin to read, and still reports Unset.
  if (typeBits == 0) return; // stays blank -> "Unset"
  if (countBits(typeBits) > 1) {
    cutType = "mixed";
  } else if (typeBits & CUTCOLOR_OUTSIDE) {
    cutType = "outside";
  } else if (typeBits & CUTCOLOR_INSIDE) {
    cutType = "inside";
  } else if (typeBits & CUTCOLOR_POCKET) {
    cutType = "pocket";
  } else if (typeBits & CUTCOLOR_ONLINE) {
    cutType = "online";
  } else if (typeBits & CUTCOLOR_GUIDE) {
    cutType = "guide";
  }
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
  // "Unset" rather than a bare "-": it's the same word the Upload
  // section's own cut type dropdown uses for this state, and it says
  // "nothing is set here" instead of leaving you to guess whether the
  // column is empty or just unknown.
  return "Unset";
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

  // Cheap pass: names, sizes and dates only. Deliberately NOT the cut
  // type - see the scan below for why that waits.
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
          entries.push_back(e);
        }
        f.close();
        f = dir.openNextFile();
      }
    }
    dir.close(); // must close all handles before storageEndAppAccess() unmounts
  }

  // Sorting, searching and paging all happen on names and dates alone, so
  // they can run here - before any file is opened - rather than after.
  // That ordering is the point: it means the expensive part below only
  // ever touches the handful of files actually about to be drawn.
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
  int startIdx = (page - 1) * FILES_PER_PAGE;
  int endIdx = min(filteredCount, startIdx + FILES_PER_PAGE);

  // Now the expensive part, for this page's rows only. readShaperInfo()
  // reads a file end to end, so doing it during the directory walk above
  // meant reading every file in the folder to render at most
  // FILES_PER_PAGE rows - roughly 800KB off flash for a 40-file folder to
  // draw ten lines, with the cost growing as the library grows. Deferring
  // it here caps the work at one page's worth no matter how many files
  // are stored. Reopening ten files by name costs far less than the reads
  // it avoids.
  for (int i = startIdx; i < endIdx; i++) {
    File f = FFat.open(joinFolder(viewFolder, filtered[i].name));
    if (!f) continue; // vanished between the walk and here - leave it blank
    readShaperInfo(f, filtered[i].cutType);
    f.close();
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
  // Column order and widths are set in style.css (.file-table): the three
  // metadata columns share one fixed width so they line up as an evenly
  // spaced block at the right, and Name takes everything left over.
  html += "<table class='file-table'><tr><th class='col-check'><input type='checkbox' id='selectAllFiles' onclick='toggleAllFiles(this)'></th><th>Name</th><th class='col-meta'>Size</th><th class='col-meta'>Cut type</th><th class='col-meta'>Uploaded</th></tr>";
  if (filteredCount == 0) {
    html += String("<tr><td colspan=5><em>") + (query.length() > 0 ? "No files match your search." : "No files yet.") + "</em></td></tr>";
  } else {
    // Deliberately reuses the startIdx/endIdx computed above rather than
    // recomputing them: those same bounds decided which files got their
    // cut type read, so if the two ever drifted apart this table would
    // draw rows whose Cut type cell was never filled in.
    for (int i = startIdx; i < endIdx; i++) {
      const FileEntry &e = filtered[i];
      bool isNew = std::find(justUploaded.begin(), justUploaded.end(), e.name) != justUploaded.end();
      // The "just uploaded" tick rides with the name rather than the date:
      // it's about this file, and the three metadata columns need to stay
      // the same width as each other to line up.
      String checkmark = isNew ? " <span style='color:#0a0' title='Just uploaded'>&#10003;</span>" : "";
      html += "<tr><td class='col-check'><input type='checkbox' class='rowcheck' name='name' value='" + htmlEscape(e.name) + "' onchange='updateBatchButtons()'></td>";
      html += "<td>" + htmlEscape(e.name) + checkmark + "</td>"
              "<td class='col-meta'>" + formatBytes(e.size) + "</td>"
              "<td class='col-meta'><button type='button' class='link-btn cutTypeCell' data-name='" + htmlEscape(e.name) + "' data-dir='" + htmlEscape(viewFolder) +
              "' onclick='openCutEditorFromBtn(this)'>" + cutTypeLabel(e.cutType) + "</button></td>"
              "<td class='col-meta'>" + formatDateTime(e.mtime) + "</td></tr>";
    }
  }
  html += "<tr><td colspan=5 style='color:#666'>" + formatBytes(used) + " used of " + formatBytes(total) + "</td></tr>";
  html += "</table>";
  // A move destination exists whenever there's somewhere other than the
  // currently-viewed folder to put files: always true once you're inside
  // any folder (root is always a valid destination), and true at root
  // once at least one folder exists.
  bool hasMoveDest = viewFolder.length() > 0 || !folders.empty();
  bool showDeleteFolder = viewFolder.length() > 0;
  if (filteredCount > 0 || showDeleteFolder) {
    // Rename/Move/Delete all start disabled - updateBatchButtons() (in
    // the page script) enables them once at least one row is checked.
    // They're split across the two ends of the .file-actions row on
    // purpose: the reversible actions (Rename, Move) group together on
    // the left, and Delete sits alone on the right, well away from them,
    // so the one that destroys work isn't the button next to the one you
    // meant to press. "Delete this folder" joins it on the right for the
    // same reason (and doesn't depend on any row being checked, since it
    // acts on the folder itself) - it's also rendered even when the
    // folder is empty (filteredCount == 0), so an empty folder can still
    // be removed from here.
    html += "<div class='file-actions'><div class='file-actions-group'>";
    if (filteredCount > 0) {
      // Rename opens a dialog instead of submitting - the new names have
      // to be typed somewhere first. See showRenamePanel().
      html += "<button type='button' id='renameBtn' onclick='showRenamePanel()' disabled>Rename</button>";
      if (hasMoveDest) {
        // "Move" doesn't submit directly either - it just reveals
        // #movePanel below (destination select + confirm), so the
        // "Move to:" dropdown doesn't sit in the way when nobody's
        // moving anything.
        html += " <button type='button' id='moveBtn' onclick='showMovePanel()' disabled>Move</button>";
      }
    }
    html += "</div><div class='file-actions-group'>";
    if (filteredCount > 0) {
      html += "<button type='submit' id='deleteBtn' formaction='/delete' onclick='return confirmBatchDelete()' disabled>Delete</button>";
    }
    if (showDeleteFolder) {
      html += "<button type='submit' formaction='/deletefolder' class='link-btn' onclick='return confirmDeleteFolder()'>Delete this folder</button>";
    }
    html += "</div>";  // .file-actions-group (right)
    html += "</div>";  // .file-actions
  }
  html += "</form>";
  if (filteredCount > 0 && hasMoveDest) {
    // Same dialog shape as Rename (see #renameOverlay in renderPage()),
    // so the two batch actions behave the same way rather than one
    // opening a modal and the other unfolding a strip below the table.
    // showMovePanel() fills in the list of what's about to move.
    //
    // Rendered outside <form id='deleteForm'>, so the destination select
    // and the confirm button each carry an explicit form='deleteForm' -
    // HTML lets a control outside a <form> submit with/into it that way,
    // which is what makes the checked rowchecks and the chosen dest
    // travel together in one POST, same formaction-override trick used
    // for Delete above. That association is by attribute, not by DOM
    // position, so it keeps working from inside the overlay.
    html += "<div id='moveOverlay' class='modal-overlay' style='display:none'>"
            "<div class='modal-box dialog-narrow'>"
            "<div class='cut-editor-header'>"
            "<h3>Move files</h3>"
            "<button type='button' class='link-btn' onclick='closeMovePanel()'>Close</button>"
            "</div>"
            "<p class='sub'>Moving:</p>"
            "<ul id='moveFileList' class='dialog-file-list'></ul>"
            "<p class='sub'>Destination folder:</p>";
    html += "<select name='dest' id='moveDest' form='deleteForm' class='full-width'>";
    if (viewFolder.length() > 0) {
      html += "<option value=''>HOME</option>";
    }
    for (size_t i = 0; i < folders.size(); i++) {
      if (folders[i] == viewFolder) continue;
      html += "<option value='" + htmlEscape(folders[i]) + "'>" + htmlEscape(folders[i]) + "</option>";
    }
    html += "</select>"
            "<br><br>"
            "<button type='submit' id='confirmMoveBtn' form='deleteForm' formaction='/move' onclick='return confirmMove()'>Move</button> "
            "<button type='button' id='cancelMoveBtn' class='link-btn' onclick='closeMovePanel()'>Cancel</button>"
            "</div>"
            "</div>";
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
          "pick a folder to upload into, and blanket-assign a Shaper cut type and depth to "
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
  html += "<select id='cutType' class='full-width'>"
          // "Default" rather than "unset": the file list uses "Unset" for a
          // file with no encoding Origin can read, which is a different
          // state from this one, and having both say the same word made a
          // DXF uploaded on "unset" look like it contradicted itself when
          // it showed up in the list as On Line.
          "<option value='' selected>Cut type: Default</option>"
          "<option value='outside'>Cut type: Outside</option>"
          "<option value='inside'>Cut type: Inside</option>"
          "<option value='pocket'>Cut type: Pocket</option>"
          "<option value='online'>Cut type: On Line</option>"
          "<option value='guide'>Cut type: Guide</option>"
          "</select>";
  html += "<br><br>";
  // "Default" leaves anchors alone entirely, which for an SVG means the
  // file is not rewritten at all - so a custom anchor drawn in another
  // program survives untouched. Anything else writes a standard-position
  // anchor, replacing whatever was there.
  html += "<select id='uploadAnchor' class='full-width'>"
          "<option value='' selected>Anchor: Default</option>"
          "<option value='tl'>Anchor: Top left</option>"
          "<option value='tr'>Anchor: Top right</option>"
          "<option value='bl'>Anchor: Bottom left</option>"
          "<option value='br'>Anchor: Bottom right</option>"
          "<option value='c'>Anchor: Center</option>";
  html += "</select>";
  html += "<br><br>";
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

  // Rename dialog: hidden by default, opened by the file list's Rename
  // button. The per-file rows are built client-side from whichever rows
  // are checked at the moment it opens (see showRenamePanel()), so this
  // is just the shell. Its own <form> posts to /rename rather than
  // borrowing #deleteForm, because the rows it submits are the "from"/
  // "to" pairs it builds itself, not the table's checkboxes. Rendered
  // unconditionally (it's hidden and nearly empty until opened) - the
  // Rename button that opens it is itself only drawn when there are files
  // to act on. lastViewedFolder is set by the renderFilesSection() call
  // above, so it already reflects the folder being shown.
  html += "<div id='renameOverlay' class='modal-overlay' style='display:none'>"
          "<div class='modal-box dialog-narrow'>"
          "<div class='cut-editor-header'>"
          "<h3>Rename files</h3>"
          "<button type='button' class='link-btn' onclick='closeRenamePanel()'>Close</button>"
          "</div>"
          "<p class='sub'>Leave a name unchanged to skip that file. File types can't be "
          "changed here.</p>"
          "<form id='renameForm' method='POST' action='/rename' onsubmit='return confirmRename()'>";
  if (lastViewedFolder.length() > 0) {
    html += "<input type='hidden' name='dir' value='" + htmlEscape(lastViewedFolder) + "'>";
  }
  html += "<div id='renameFields'></div>"
          "<button type='submit' id='confirmRenameBtn'>Rename</button> "
          "<button type='button' class='link-btn' onclick='closeRenamePanel()'>Cancel</button>"
          "</form>"
          "</div>"
          "</div>";

  // Per-line cut editor: hidden by default, opened from a "Cut type" cell
  // button in the file list above (see cutTypeCell/openCutEditorFromBtn).
  // Fetches the real file's SVG from the new /svg route, renders it, and
  // lets the user click individual paths to give them their own cut
  // type and depth instead of the whole-file value the Upload
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
          "<div><span class='swatch' style='background:#e11d2e'></span>Anchor</div>"
          "<div><span class='swatch' style='background:#00b8ff'></span>Selected</div>"
          "</div>" // .cut-legend
          "</div>" // .cut-editor-viewer
          "<div class='cut-editor-panel'>"
          // Says up front whether this file carries a Shaper custom anchor.
          // The anchor is already drawn in red and protected from editing,
          // but a red triangle tucked into a corner of a busy drawing is
          // easy to miss, and "is my anchor still in here?" is exactly the
          // question someone opens this dialog worrying about.
          "<p id='cutEditorAnchorInfo' class='sub'></p>"
          // The info line above reports what the file HAS; this sets it.
          // Kept separate on purpose - a control that both reports and
          // changes state reads as though "Top right" is already true.
          "<select id='editAnchor' class='full-width' onchange='handleEditAnchorChange()'>"
          "<option value='keep' selected>Anchor: leave as is</option>"
          "<option value='tl'>Anchor: Top left</option>"
          "<option value='tr'>Anchor: Top right</option>"
          "<option value='bl'>Anchor: Bottom left</option>"
          "<option value='br'>Anchor: Bottom right</option>"
          "<option value='c'>Anchor: Center</option>"
          "<option value='none'>Anchor: remove</option>"
          "</select>"
          "<br><br>"
          "<p id='cutEditorSelCount' class='sub'>No line selected.</p>"
          "<select id='editCutType' class='full-width'>"
          "<option value='' selected>Cut type: choose one</option>"
          "<option value='outside'>Cut type: Outside</option>"
          "<option value='inside'>Cut type: Inside</option>"
          "<option value='pocket'>Cut type: Pocket</option>"
          "<option value='online'>Cut type: On Line</option>"
          "<option value='guide'>Cut type: Guide</option>"
          "</select>"
          "<br><br>"
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
          "var renameBtn = document.getElementById('renameBtn');"
          "if (delBtn) delBtn.disabled = !anyChecked;"
          "if (moveBtn) moveBtn.disabled = !anyChecked;"
          "if (renameBtn) renameBtn.disabled = !anyChecked;"
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
          // Splits "part.v2.svg" into {stem: 'part.v2', ext: '.svg'}. The
          // dot > 0 test keeps a leading-dot name from being read as all
          // extension and no name.
          "function splitFileName(name) {"
          "var dot = name.lastIndexOf('.');"
          "if (dot > 0) return { stem: name.substring(0, dot), ext: name.substring(dot) };"
          "return { stem: name, ext: '' };"
          "}"
          // Builds one labelled row per checked file: a hidden "from" with
          // the current name (so handleRename() can match old to new by
          // position), a text field holding ONLY the name part, and the
          // extension shown beside it as fixed text rather than as
          // editable characters - renaming a file is about the name, and
          // an .svg quietly turned into .svh is a file the Origin stops
          // seeing. The submitted "to" is its own hidden field, assembled
          // from stem + extension in confirmRename(); the visible field
          // has no name attribute, so it never posts on its own. Rebuilt
          // from scratch on every open rather than kept in sync, since
          // the selection can change freely while the dialog is closed.
          "function showRenamePanel() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to rename.'); return; }"
          "var wrap = document.getElementById('renameFields');"
          "wrap.innerHTML = '';"
          "for (var i = 0; i < boxes.length; i++) {"
          "var name = boxes[i].value;"
          "var parts = splitFileName(name);"
          "var row = document.createElement('div');"
          "row.className = 'rename-row';"
          "var hidden = document.createElement('input');"
          "hidden.type = 'hidden'; hidden.name = 'from'; hidden.value = name;"
          "var target = document.createElement('input');"
          "target.type = 'hidden'; target.name = 'to'; target.value = name;"
          "var label = document.createElement('label');"
          "label.className = 'sub';"
          "label.textContent = name;"
          "var field = document.createElement('div');"
          "field.className = 'rename-field';"
          "var input = document.createElement('input');"
          "input.type = 'text'; input.className = 'rename-stem'; input.value = parts.stem;"
          "input.setAttribute('maxlength', '60');"
          "input.setAttribute('aria-label', 'New name for ' + name);"
          "field.appendChild(input);"
          "if (parts.ext) {"
          "var ext = document.createElement('span');"
          "ext.className = 'rename-ext';"
          "ext.textContent = parts.ext;"
          "field.appendChild(ext);"
          "}"
          "row.appendChild(hidden); row.appendChild(target); row.appendChild(label); row.appendChild(field);"
          "wrap.appendChild(row);"
          "}"
          "document.getElementById('renameOverlay').style.display = 'flex';"
          "var first = wrap.querySelector('.rename-stem');"
          "if (first) { first.focus(); first.select(); }"
          "}"
          "function closeRenamePanel() {"
          "document.getElementById('renameOverlay').style.display = 'none';"
          "}"
          // Assembles each row's submitted name from the stem the user
          // typed plus its untouched extension, and catches the mistakes
          // worth catching before a round trip. The device re-validates
          // all of it regardless (see handleRename()).
          "function confirmRename() {"
          "var rows = document.querySelectorAll('#renameFields .rename-row');"
          "var seen = {};"
          "var changed = 0;"
          "for (var i = 0; i < rows.length; i++) {"
          "var stemInput = rows[i].querySelector('.rename-stem');"
          "var extEl = rows[i].querySelector('.rename-ext');"
          "var original = rows[i].querySelector('input[name=\"from\"]');"
          "var target = rows[i].querySelector('input[name=\"to\"]');"
          "var stem = stemInput.value.replace(/^\\s+|\\s+$/g, '');"
          "if (stem.length === 0) { alert('A file name cannot be blank.'); stemInput.focus(); return false; }"
          "if (stem.indexOf('/') >= 0 || stem.indexOf('\\\\') >= 0) { alert('File names cannot contain slashes.'); stemInput.focus(); return false; }"
          "var full = stem + (extEl ? extEl.textContent : '');"
          "var key = full.toLowerCase();"
          "if (seen[key]) { alert('Two files would both be named ' + full + '.'); stemInput.focus(); return false; }"
          "seen[key] = true;"
          "target.value = full;"
          "if (original.value !== full) changed++;"
          "}"
          "if (changed === 0) { alert('No names were changed.'); return false; }"
          "return true;"
          "}"
          // Move opens the same kind of dialog as Rename rather than
          // unfolding a strip below the table, so both batch actions
          // behave alike. Listing the files being moved inside the dialog
          // also replaces the old confirm() step: the dialog already
          // shows exactly what is about to move and where, and moving is
          // reversible anyway - unlike Delete, which keeps its confirm.
          "function showMovePanel() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to move.'); return; }"
          "var list = document.getElementById('moveFileList');"
          "list.innerHTML = '';"
          "for (var i = 0; i < boxes.length; i++) {"
          "var li = document.createElement('li');"
          "li.textContent = boxes[i].value;"
          "list.appendChild(li);"
          "}"
          "document.getElementById('moveOverlay').style.display = 'flex';"
          "}"
          "function closeMovePanel() {"
          "document.getElementById('moveOverlay').style.display = 'none';"
          "}"
          "function confirmMove() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to move.'); return false; }"
          "return true;"
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
          // -----------------------------------------------------------
          // THE thing that actually makes the Origin honor a cut type.
          //
          // Shaper Origin reads cut type from each shape's FILL and
          // STROKE COLOR - not from the shaper:* namespace attributes.
          // Shaper's own docs call this "cut type encoding" and say
          // Origin "accepts color-coded vector shapes indicating cut
          // types"; their Inkscape guide states plainly that a gray
          // stroke means an On Line cut and a gray fill means a Pocket
          // cut. The shaper:* attributes are Shaper Studio's own
          // metadata (plus the documented shaper:cutDepth override) and
          // ride along in its exports, but they are NOT what selects the
          // cut type on the machine.
          //
          // This app previously wrote only the shaper:* attributes and
          // left every path at dxf2svg's default black stroke /
          // fill:none - which is not a recognized encoding, so the
          // Origin fell back to its default for every shape no matter
          // how correct those attributes were. That is why a whole
          // series of real-hardware tests kept showing "On Line" or
          // "unset" even after the saved file itself was byte-perfect.
          //
          // Verified against a real Shaper Studio export of the user's
          // own design (testFiles/testPart2_Shaper.svg): its pocket path
          // carries fill="#7F7F7F" with no stroke, its outside path
          // fill="#000000" with no stroke, and every untouched path
          // stroke="#7F7F7F" with fill="none" - matching the table
          // below exactly. Origin matches these by color, tolerantly
          // (their gray guide just says "make R, G and B equal"), not by
          // exact hex, but we write the canonical values Shaper's own
          // tools emit.
          "var SHAPER_CUT_COLORS = {"
          "outside: {fill: '#000000', stroke: 'none'},"
          "inside: {fill: '#FFFFFF', stroke: '#000000'},"
          "pocket: {fill: '#7F7F7F', stroke: 'none'},"
          "online: {fill: 'none', stroke: '#7F7F7F'},"
          "guide: {fill: 'none', stroke: '#0068FF'}"
          "};"
          // Applies that encoding to one shape as real presentation
          // attributes. Deliberately NOT inline style: the per-line
          // editor uses inline style for its own on-screen preview
          // coloring and strips every style="" off the clone before
          // saving (see saveCutEditor), so writing these as attributes
          // keeps the preview and the saved-file encoding cleanly
          // separate - the editor still shows thin colored outlines
          // while the file itself carries Shaper's real colors.
          "function applyShaperCutColors(el, cutType) {"
          "var c = SHAPER_CUT_COLORS[cutType];"
          "if (!c) return;"
          "el.setAttribute('fill', c.fill);"
          "el.setAttribute('stroke', c.stroke);"
          "}"
          // No tool diameter here any more. Origin takes the bit size from
          // what you tell it at the machine - the only thing that knows
          // what's actually in the collet - and testing showed a file's
          // shaper:toolDia never moved that setting, across many uploads
          // where the file said 3.175mm and the machine stayed on 6.35mm.
          // Shaper's own exports write it onto On Line paths too, where
          // there is no offset to compute and it cannot mean anything,
          // which reads as "the bit this design was drawn for" rather than
          // an instruction. So it was a required field that changed
          // nothing, and shaper:cutOffset alongside it was always "0in".
          // shaper:cutDepth stays - that one Shaper documents as a real
          // override, and it's a different thing that merely shares the
          // namespace.
          // ---- Shaper custom anchors ------------------------------------
          // A custom anchor is a red right-angled triangle: the right-angle
          // vertex is the design's reference point, the shorter leg is the
          // X axis and the longer leg the Y axis.
          //
          // SIZE does not matter - "that right angle and it being red is
          // key, nothing else to it", from Beau, who uses these on a real
          // Origin. So the legs are sized purely for our own benefit: 5% of
          // the drawing's smaller dimension for X, double for Y, which
          // keeps the triangle proportionate and visible in the editor's
          // preview whether the design is a coaster or a table top.
          //
          // The 2:1 ratio is NOT about size. It is about keeping "shorter
          // leg" unambiguous: equal legs would leave nothing to say which
          // one is X.
          //
          //   ASSUMPTION (direction): the docs say which leg is which axis,
          //   never which way either points, and Beau's note doesn't reach
          //   that far. The legs are drawn INWARD from the chosen corner,
          //   so the triangle always sits inside the design's bounds. If a
          //   design places but comes out rotated or mirrored, this is the
          //   line to change. It may well not matter at all - the docs say
          //   the anchor's axes align to the Grid, or to the Origin's
          //   screen when there is no Grid, which would mean the triangle
          //   supplies the point and the workspace supplies the
          //   orientation.
          //
          // What is NOT assumed: the fill. "#FF0000" is confirmed working
          // by a user who got one recognized, and the same thread found
          // Shaper's match is case-sensitive - "red" and "#FF0000" work,
          // "Red" and "RED" do not, contrary to the SVG spec. Do not
          // "tidy" this into a named color.
          //
          // A malformed anchor makes the Origin refuse the file outright
          // ("unable to place design"), so a wrong guess is loud rather
          // than silent - except for direction, where a well-formed but
          // rotated anchor would be accepted and simply be wrong.
          "var ANCHOR_FILL = '#FF0000';"
          // Tags every red-filled shape so the rest of the code can leave
          // anchors alone. Needs the element rendered - see
          // withRenderedSvg() - because it reads the computed fill.
          "function markAnchors(svgEl) {"
          "var shapes = svgEl.querySelectorAll('path,rect,circle,ellipse,polygon,polyline,line');"
          "for (var i = 0; i < shapes.length; i++) {"
          "if (cutEditorIsAnchor(shapes[i])) shapes[i].setAttribute('data-anchor', '1');"
          "}"
          "}"
          // The drawing's own bounds, ignoring any existing anchor and the
          // editor's invisible hit proxies - an anchor must be placed
          // relative to the design, not to a previous anchor.
          "function svgContentBox(svgEl) {"
          "var shapes = svgEl.querySelectorAll('path,rect,circle,ellipse,polygon,polyline,line');"
          "var minX = null, minY = null, maxX = null, maxY = null;"
          "for (var i = 0; i < shapes.length; i++) {"
          "var el = shapes[i];"
          "if (el.getAttribute('data-anchor') || el.getAttribute('data-hit-proxy')) continue;"
          "var b = null;"
          "try { b = el.getBBox(); } catch (e) { continue; }"
          "if (!b || (b.width === 0 && b.height === 0)) continue;"
          "if (minX === null || b.x < minX) minX = b.x;"
          "if (minY === null || b.y < minY) minY = b.y;"
          "if (maxX === null || b.x + b.width > maxX) maxX = b.x + b.width;"
          "if (maxY === null || b.y + b.height > maxY) maxY = b.y + b.height;"
          "}"
          "if (minX === null) return null;"
          "return {x: minX, y: minY, w: maxX - minX, h: maxY - minY};"
          "}"
          // Note SVG's y axis points DOWN, so 'top' is the smaller y.
          "function buildAnchorPath(doc, box, pos) {"
          "var legX = Math.max(Math.min(box.w, box.h) * 0.05, 0.5);"
          "var legY = legX * 2;"
          "var vx, vy, sx, sy;"
          "if (pos === 'tl') { vx = box.x; vy = box.y; sx = 1; sy = 1; }"
          "else if (pos === 'tr') { vx = box.x + box.w; vy = box.y; sx = -1; sy = 1; }"
          "else if (pos === 'bl') { vx = box.x; vy = box.y + box.h; sx = 1; sy = -1; }"
          "else if (pos === 'br') { vx = box.x + box.w; vy = box.y + box.h; sx = -1; sy = -1; }"
          "else { vx = box.x + box.w / 2; vy = box.y + box.h / 2; sx = 1; sy = 1; }"
          "var r = function(n) { return Math.round(n * 1000) / 1000; };"
          "var d = 'M ' + r(vx) + ',' + r(vy) + ' L ' + r(vx + sx * legX) + ',' + r(vy) +"
          "' L ' + r(vx) + ',' + r(vy + sy * legY) + ' Z';"
          "var p = doc.createElementNS('http://www.w3.org/2000/svg', 'path');"
          "p.setAttribute('d', d);"
          "p.setAttribute('fill', ANCHOR_FILL);"
          "p.setAttribute('stroke', 'none');"
          "return p;"
          "}"
          // Replaces whatever anchor the file had - Shaper allows only one
          // per object, so adding without removing would be invalid.
          "function setAnchor(svgEl, pos) {"
          "var existing = svgEl.querySelectorAll('[data-anchor]');"
          "for (var i = 0; i < existing.length; i++) existing[i].parentNode.removeChild(existing[i]);"
          "if (!pos) return true;"
          "var box = svgContentBox(svgEl);"
          "if (!box || box.w <= 0 || box.h <= 0) return false;"
          "var p = buildAnchorPath(svgEl.ownerDocument || document, box, pos);"
          "p.setAttribute('data-anchor', '1');"
          "svgEl.appendChild(p);"
          "return true;"
          "}"
          // getBBox() and getComputedStyle() both return nothing useful for
          // an element that was parsed but never laid out, so anything that
          // needs either has to happen while the SVG is attached. Parks it
          // offscreen, runs fn, then takes the holder away again.
          "function withRenderedSvg(svgEl, fn) {"
          "var holder = document.createElement('div');"
          "holder.setAttribute('style', 'position:absolute;left:-10000px;top:0;width:800px');"
          "document.body.appendChild(holder);"
          "holder.appendChild(svgEl);"
          "try { return fn(); } finally { holder.parentNode.removeChild(holder); }"
          "}"
          "function stripAnchorMarks(svgEl) {"
          "var marked = svgEl.querySelectorAll('[data-anchor]');"
          "for (var i = 0; i < marked.length; i++) marked[i].removeAttribute('data-anchor');"
          "}"
          "function applyShaperMetadata(svgText, cutType, depthVal, depthUnit, anchorPos) {"
          "var doc = new DOMParser().parseFromString(svgText, 'image/svg+xml');"
          "if (doc.querySelector('parsererror')) { throw new Error('Could not parse SVG'); }"
          "var svgEl = doc.documentElement;"
          "var SHAPER_NS = 'http://www.shapertools.com/namespaces/shaper';"
          "var XMLNS_NS = 'http://www.w3.org/2000/xmlns/';"
          "svgEl.setAttributeNS(XMLNS_NS, 'xmlns:shaper', SHAPER_NS);"
          "var depthAttr = null;"
          "if (depthVal !== '' && depthVal !== null && !isNaN(parseFloat(depthVal))) {"
          "depthAttr = parseFloat(depthVal) + depthUnit;"
          "}"
          "return withRenderedSvg(svgEl, function() {"
          // Find the anchors FIRST, so the cut-type loop below can skip
          // them. Without this, uploading an Affinity file that already
          // had an anchor while also choosing a blanket cut type would
          // recolor the anchor and destroy it - the same bug the per-line
          // editor had, on the other code path.
          "markAnchors(svgEl);"
          "var shapes = svgEl.querySelectorAll('path,rect,circle,ellipse,polygon,polyline,line');"
          "for (var i = 0; i < shapes.length; i++) {"
          "if (shapes[i].getAttribute('data-anchor')) continue;"
          "if (cutType) {"
          "shapes[i].setAttributeNS(SHAPER_NS, 'shaper:cutType', cutType);"
          "applyShaperCutColors(shapes[i], cutType);"
          "}"
          "if (depthAttr) shapes[i].setAttributeNS(SHAPER_NS, 'shaper:cutDepth', depthAttr);"
          "}"
          // '' means "leave whatever anchor the file has alone".
          "if (anchorPos) setAnchor(svgEl, anchorPos === 'none' ? '' : anchorPos);"
          "stripAnchorMarks(svgEl);"
          "return new XMLSerializer().serializeToString(svgEl);"
          "});"
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
          "var EDITOR_ANCHOR_COLOR = '#e11d2e';"
          // A Shaper "custom anchor" is not a cut - it's a right-angled
          // triangle with a RED fill and no stroke, whose right-angle
          // vertex tells Origin where the design's reference point is and
          // whose legs define its axes. Red is the whole mechanism, same
          // as gray means On Line. That matters here because the editor
          // draws anything without a cutType in black, so an anchor looked
          // exactly like a line nobody had set yet - click it, hit Apply,
          // and applyShaperCutColors() would overwrite the red and quietly
          // turn the file's anchor into a triangle the Origin cuts.
          //
          // Read through getComputedStyle so a named color ('red'), a hex
          // and an rgb() all arrive in the same normalized form, and read
          // it BEFORE cutEditorInitShape() forces fill:none for preview.
          // Deliberately liberal - any red-dominant fill counts, not only
          // a strict right triangle. A red shape encodes no cut type
          // whatever its geometry, so declining to overwrite it is the
          // right call even when it isn't a real anchor.
          "function cutEditorIsAnchor(el) {"
          "var f = window.getComputedStyle(el).fill || '';"
          "var m = /rgba?\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)/.exec(f);"
          "if (!m) return false;"
          "var r = +m[1], g = +m[2], b = +m[3];"
          "return r >= 128 && r > g + 60 && r > b + 60;"
          "}"
          "var cutEditorState = {name: '', folder: '', svgEl: null, selected: [], dirty: false};"
          "function openCutEditorFromBtn(btn) {"
          "openCutEditor(btn.getAttribute('data-name'), btn.getAttribute('data-dir'));"
          "}"
          "function openCutEditor(name, folder) {"
          "cutEditorState = {name: name, folder: folder, svgEl: null, selected: [], dirty: false};"
          "document.getElementById('cutEditorTitle').textContent = 'Edit cut types: ' + name;"
          "document.getElementById('cutEditorSvgWrap').innerHTML = '<p class=\"sub\">Loading...</p>';"
          "document.getElementById('cutEditorStatus').textContent = '';"
          "document.getElementById('cutEditorAnchorInfo').textContent = '';"
          "document.getElementById('editCutType').value = '';"
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
          "updateAnchorInfo();"
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
          // Check the real fill before overriding it below.
          "if (cutEditorIsAnchor(el)) el.setAttribute('data-anchor', '1');"
          "el.style.fill = (el.getAttribute('data-anchor') ? EDITOR_ANCHOR_COLOR : 'none');"
          "cutEditorRecolor(el);"
          "var hit = el.cloneNode(false);"
          "hit.setAttribute('data-hit-proxy', '1');"
          // cloneNode copies data-anchor along with everything else, which
          // would make every [data-anchor] query count the invisible proxy
          // as a second anchor - and Shaper allows only one. The marker
          // belongs to the real shape only.
          "hit.removeAttribute('data-anchor');"
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
          // Splits a written shaper:cutDepth value like '9mm' into its
          // numeric and unit parts. Written without a space between them,
          // matching the format Shaper's own documented cut depth
          // encoding uses. Still tolerates an optional space, so a file
          // saved by an older build of this app - which wrote "9 mm" -
          // reads back correctly instead of silently losing its depth.
          "function parseValueUnit(str) {"
          "var m = /^(-?[0-9.]+)\\s*([a-zA-Z]*)$/.exec(str || '');"
          "return m ? { val: m[1], unit: m[2] } : { val: '', unit: '' };"
          "}"
          "function cutEditorRecolor(el) {"
          "if (el.getAttribute('data-anchor')) {"
          "el.style.fill = EDITOR_ANCHOR_COLOR;"
          "el.style.stroke = EDITOR_ANCHOR_COLOR;"
          "el.style.strokeWidth = '2';"
          "el.style.strokeDasharray = '';"
          "return;"
          "}"
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
          // Refuse at the point of selection rather than at Apply: there is
          // nothing you could usefully set on an anchor, so letting it into
          // a selection would only create a way to damage the file.
          "if (el.getAttribute('data-anchor')) {"
          "document.getElementById('cutEditorStatus').textContent = "
          "'That red shape is the file\\'s custom anchor - it tells the Origin where to place the design, so it has no cut type and can\\'t be given one.';"
          "return;"
          "}"
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
          "function handleEditAnchorChange() {"
          "var sel = document.getElementById('editAnchor');"
          "var pos = sel.value;"
          "if (pos === 'keep' || !cutEditorState.svgEl) return;"
          "var had = cutEditorState.svgEl.querySelectorAll('[data-anchor]').length > 0;"
          // Replacing a hand-made anchor throws away geometry the user
          // positioned deliberately, and it cannot be undone from here, so
          // it is worth one confirmation rather than a silent swap.
          "if (had) {"
          "var msg = (pos === 'none')"
          "? 'Remove the custom anchor from this file?'"
          ": 'This file already has a custom anchor. Replace it with a standard one? The original position will be lost.';"
          "if (!confirm(msg)) { sel.value = 'keep'; return; }"
          "}"
          "var ok = setAnchor(cutEditorState.svgEl, pos === 'none' ? '' : pos);"
          "if (!ok) {"
          "document.getElementById('cutEditorStatus').textContent = 'Could not work out where the drawing is, so no anchor was placed.';"
          "sel.value = 'keep';"
          "return;"
          "}"
          "var added = cutEditorState.svgEl.querySelector('[data-anchor]');"
          "if (added) cutEditorInitShape(added);"
          "cutEditorState.dirty = true;"
          "sel.value = 'keep';"
          "updateAnchorInfo();"
          "document.getElementById('cutEditorStatus').textContent = (pos === 'none')"
          "? 'Anchor removed - click Save changes to write this to the file.'"
          ": 'Anchor placed - click Save changes to write this to the file.';"
          "}"
          "function updateAnchorInfo() {"
          "var el = document.getElementById('cutEditorAnchorInfo');"
          "if (!cutEditorState.svgEl) { el.textContent = ''; return; }"
          "var found = cutEditorState.svgEl.querySelectorAll('[data-anchor]').length;"
          "el.textContent = found"
          "? 'Anchor: custom anchor in this file (shown in red) - it will be left exactly as it is.'"
          ": 'Anchor: none in this file. The Origin will use whichever of its own anchor points you pick on the tool.';"
          "}"
          "function updateSelectionSummary() {"
          "var n = cutEditorState.selected.length;"
          "document.getElementById('cutEditorSelCount').textContent = n === 0 ? 'No line selected.' : (n + ' line(s) selected.');"
          "var applyBtn = document.getElementById('applyToSelectedBtn');"
          "if (applyBtn) applyBtn.disabled = (n === 0);"
          "if (n > 0) {"
          "var el = cutEditorState.selected[0];"
          // A shape with nothing set yet gets On Line preselected rather
          // than a blank "choose one". On Line is what the Origin treats
          // an unmarked line as anyway - and it's what a plain converted
          // file already encodes, since dxf2svg emits Shaper's On Line
          // gray on every path - so this shows you what the shape
          // already is instead of pretending it's undecided. Applying it
          // then just makes that explicit. Anything with a real cut type
          // still shows its own.
          "document.getElementById('editCutType').value = cutEditorCutTypeOf(el) || 'online';"
          "var depth = el.getAttributeNS(EDITOR_SHAPER_NS, 'cutDepth') || '';"
          "var depthParsed = parseValueUnit(depth);"
          "document.getElementById('editDepth').value = depthParsed.val || '';"
          "if (depthParsed.unit) document.getElementById('editDepthUnit').value = depthParsed.unit;"
          "}"
          "}"
          "function applyToSelectedShapes() {"
          "var cutType = document.getElementById('editCutType').value;"
          "if (!cutType) { alert('Choose a cut type first.'); return; }"
          "if (cutEditorState.selected.length === 0) { alert('Select at least one line first.'); return; }"
          "var depthVal = document.getElementById('editDepth').value;"
          "var depthUnit = document.getElementById('editDepthUnit').value;"
          "var depthAttr = null;"
          "if (depthVal !== '' && !isNaN(parseFloat(depthVal))) depthAttr = parseFloat(depthVal) + depthUnit;"
          "var sel = cutEditorState.selected.slice();"
          "for (var i = 0; i < sel.length; i++) {"
          "var el = sel[i];"
          "el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:cutType', cutType);"
          // The part the Origin actually reads - see applyShaperCutColors.
          "applyShaperCutColors(el, cutType);"
          "if (depthAttr) el.setAttributeNS(EDITOR_SHAPER_NS, 'shaper:cutDepth', depthAttr);"
          // Clear any tool diameter left on the shape by an older build,
          // so re-saving a file cleans it out rather than preserving a
          // field this app no longer writes or reads.
          "el.removeAttributeNS(EDITOR_SHAPER_NS, 'toolDia');"
          "el.removeAttributeNS(EDITOR_SHAPER_NS, 'cutOffset');"
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
          // data-anchor is ours, for preview and protection only - it has
          // no meaning to Shaper and must not end up in the saved file.
          "stripAnchorMarks(cloneEl);"
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
          "var anchorPos = document.getElementById('uploadAnchor').value;"
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
          "if (cutType || cutDepthVal || anchorPos) svgOut = applyShaperMetadata(svgOut, cutType, cutDepthVal, cutDepthUnit, anchorPos);"
          "jobs.push({name: f.name, svgName: svgName, blob: new Blob([svgOut], {type: 'image/svg+xml'}), msg: msg});"
          "} else {"
          "if (cutType || cutDepthVal || anchorPos) {"
          "var svgText = await f.text();"
          "svgText = applyShaperMetadata(svgText, cutType, cutDepthVal, cutDepthUnit, anchorPos);"
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

// Renames any number of files in one POST, from the Rename dialog (see
// showRenamePanel() in the page script). The form emits a matched pair of
// fields per file - a hidden "from" with the current name and a text "to"
// with whatever the user typed - in that order, so the two lists line up
// by index here. Anything the user left alone is a no-op and skipped
// silently rather than reported, so renaming two files out of six checked
// says "Renamed 2 files" and not "failed: 4".
static void handleRename() {
  String folder = server.hasArg("dir") ? server.arg("dir") : "";
  if (folder.length() > 0 && !isValidFolderName(folder)) {
    folder = "";
  }
  String redirectTo = folder.length() > 0 ? ("/?dir=" + urlEncode(folder)) : "/";

  std::vector<String> fromNames;
  std::vector<String> toNames;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "from") {
      fromNames.push_back(basenameOf(server.arg(i)));
    } else if (server.argName(i) == "to") {
      String t = basenameOf(server.arg(i));
      t.trim();
      toNames.push_back(t);
    }
  }
  size_t pairCount = min(fromNames.size(), toNames.size());
  if (pairCount == 0) {
    server.sendHeader("Location", redirectTo);
    server.send(303);
    return;
  }

  ledSet(LED_BLINK_FAST); // busy

  std::vector<String> renamed;
  std::vector<String> failed;
  if (storageBeginAppAccess()) {
    // Targets claimed earlier in this same batch, so two files can't be
    // renamed onto each other - FFat.exists() alone wouldn't catch that,
    // since the first rename creates the very file the second collides
    // with only after it has already happened.
    std::vector<String> claimed;
    for (size_t i = 0; i < pairCount; i++) {
      String from = fromNames[i];
      String to = toNames[i];
      if (from.length() == 0) continue;
      if (to.length() == 0 || to == from) continue; // left alone -> nothing to do
      // Keep the original extension when the user didn't type one - it's
      // an easy thing to forget, and the Origin only reads .svg.
      if (extensionOf(to).length() == 0) {
        to += extensionOf(from);
      }
      if (!isValidFileName(to)) {
        failed.push_back(from + " (name not allowed)");
        continue;
      }
      bool alreadyClaimed = false;
      for (size_t c = 0; c < claimed.size(); c++) {
        if (claimed[c] == to) { alreadyClaimed = true; break; }
      }
      if (alreadyClaimed) {
        failed.push_back(from + " (two files renamed to " + to + ")");
        continue;
      }
      String fromPath = joinFolder(folder, from);
      String toPath = joinFolder(folder, to);
      if (!FFat.exists(fromPath)) {
        failed.push_back(from + " (no longer there)");
      } else if (FFat.exists(toPath)) {
        failed.push_back(from + " (" + to + " already exists)");
      } else if (FFat.rename(fromPath, toPath)) {
        renamed.push_back(from + " -> " + to);
        claimed.push_back(to);
      } else {
        failed.push_back(from);
      }
    }
    storageEndAppAccess(true);
  } else {
    failed = fromNames;
  }

  if (renamed.empty() && failed.empty()) {
    flashMessage = ""; // nothing was actually changed
  } else if (failed.empty()) {
    flashMessage = renamed.size() == 1 ? ("Renamed " + renamed[0])
                                       : ("Renamed " + String(renamed.size()) + " files");
    flashIsError = false;
  } else if (renamed.empty()) {
    flashMessage = "Could not rename: " + joinNames(failed);
    flashIsError = true;
  } else {
    flashMessage = "Renamed " + String(renamed.size()) + " file(s), failed: " + joinNames(failed);
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
  server.on("/rename", HTTP_POST, handleRename);
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
