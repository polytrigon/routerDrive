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

static String joinFolder(const String &basename) {
  String folder = SVG_FOLDER;
  if (!folder.endsWith("/")) {
    folder += "/";
  }
  return folder + basename;
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
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmVal);
  return String(buf);
}

// ---------------------------------------------------------------------------
// page rendering
// ---------------------------------------------------------------------------

struct FileEntry {
  String name;
  size_t size;
  time_t mtime;
};

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

  std::vector<FileEntry> entries;
  {
    File dir = FFat.open(SVG_FOLDER);
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
  size_t used = FFat.usedBytes();
  size_t total = FFat.totalBytes();
  storageEndAppAccess(false); // just reading, nothing changed - no need to nudge the host

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

  if (showControls) {
    html += "<form method='GET' action='/' class='search'>";
    html += "<input type='text' name='q' placeholder='Search files...' value='" + htmlEscape(query) + "'> ";
    html += "<button type='submit'>Search</button>";
    if (query.length() > 0) {
      html += " <a href='/'>Clear</a>";
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
  // every repeated "name" field instead of assuming exactly one.
  html += "<form id='deleteForm' method='POST' action='/delete' onsubmit=\"return confirmBatchDelete()\">";
  html += "<table><tr><th><input type='checkbox' id='selectAllFiles' onclick='toggleAllFiles(this)'></th><th>Name</th><th>Size</th><th>Uploaded</th></tr>";
  if (filteredCount == 0) {
    html += String("<tr><td colspan=4><em>") + (query.length() > 0 ? "No files match your search." : "No files yet.") + "</em></td></tr>";
  } else {
    int startIdx = (page - 1) * FILES_PER_PAGE;
    int endIdx = min(filteredCount, startIdx + FILES_PER_PAGE);
    for (int i = startIdx; i < endIdx; i++) {
      const FileEntry &e = filtered[i];
      bool isNew = std::find(justUploaded.begin(), justUploaded.end(), e.name) != justUploaded.end();
      String checkmark = isNew ? " <span style='color:#0a0' title='Just uploaded'>&#10003;</span>" : "";
      html += "<tr><td><input type='checkbox' class='rowcheck' name='name' value='" + htmlEscape(e.name) + "'></td>";
      html += "<td>" + htmlEscape(e.name) + "</td><td>" + formatBytes(e.size) + "</td><td>" + formatDateTime(e.mtime) + checkmark + "</td></tr>";
    }
  }
  html += "<tr><td colspan=4 style='color:#666'>" + formatBytes(used) + " used of " + formatBytes(total) + "</td></tr>";
  html += "</table>";
  if (filteredCount > 0) {
    html += "<button type='submit'>Delete selected</button>";
  }
  html += "</form>";

  if (showControls && totalPages > 1) {
    String qParam = query.length() > 0 ? ("&q=" + urlEncode(query)) : "";
    html += "<p class='pager'>";
    if (page > 1) {
      html += "<a href='/?page=" + String(page - 1) + qParam + "'>&laquo; Prev</a> ";
    }
    html += "Page " + String(page) + " of " + String(totalPages) + " ";
    if (page < totalPages) {
      html += "<a href='/?page=" + String(page + 1) + qParam + "'>Next &raquo;</a>";
    }
    html += "</p>";
  }

  // Every existing filename (not just this page's/search's slice), for
  // the overwrite-warning check in both upload paths below. Small enough
  // (file names, not contents) to just inline as a JS array.
  html += "<script>var existingFiles = [";
  for (size_t i = 0; i < entries.size(); i++) {
    if (i > 0) html += ",";
    html += "'" + jsStringEscape(entries[i].name) + "'";
  }
  html += "];</script>";

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
          "converted to the SVG format the Origin requires (handles most shapes, but not text - convert "
          "externally and re-select the result here if one doesn't come out clean); SVGs just upload as-is, so "
          "the units dropdown only matters for DXFs. You can select more than one file at once.</p>";
  html += "<input type='file' id='uploadFile' accept='.dxf,.svg' multiple> ";
  html += "<select id='dxfUnit' class='full-width'><option value='mm' selected>Units: mm</option><option value='in'>Units: inches</option></select>";
  html += "<br><br>";
  html += "<button type='button' id='uploadBtn' class='full-width'>Convert &amp; upload</button>";
  html += "<p id='uploadStatus' class='sub'></p>";

  html += "<h2>Restart to update Origin's import list</h2>";
  html += "<p class='sub'>Once you've uploaded files, you'll need to restart RouterDrive before they propegate "
          "in the Shaper Origin's import list. Click the Restart button below - it takes a few seconds and "
          "drops Wi-Fi briefly, but this page reconnects on its own once it's back. If restart ever doesn't do "
          "it, a physical unplug/replug of the USB cable always will.</p>";
  html += "<form method='POST' action='/rescan' onsubmit=\"return confirm('Restart RouterDrive? Wi-Fi will drop for a few seconds.')\"><button type='submit' class='full-width'>Restart RouterDrive</button></form>";

  html += renderWifiSection();

  html += "<script src='/dxf2svg.js'></script>";
  html += "<script>"
          "function toggleAllFiles(cb) {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck');"
          "for (var i = 0; i < boxes.length; i++) boxes[i].checked = cb.checked;"
          "}"
          "function confirmBatchDelete() {"
          "var boxes = document.querySelectorAll('#deleteForm .rowcheck:checked');"
          "if (boxes.length === 0) { alert('Select at least one file to delete.'); return false; }"
          "if (boxes.length === 1) return confirm('Delete ' + boxes[0].value + '?');"
          "return confirm('Delete ' + boxes.length + ' files?');"
          "}"
          // existingFiles comes from the <script> the Files section emits,
          // earlier in this same page.
          "function findOverwriteConflicts(names) {"
          "var conflicts = [];"
          "for (var i = 0; i < names.length; i++) {"
          "if (existingFiles.indexOf(names[i]) !== -1) conflicts.push(names[i]);"
          "}"
          "return conflicts;"
          "}"
          "document.getElementById('uploadBtn').addEventListener('click', async function() {"
          "var fileInput = document.getElementById('uploadFile');"
          "var status = document.getElementById('uploadStatus');"
          "if (!fileInput.files.length) { status.textContent = 'Choose a file first.'; return; }"
          "var files = Array.prototype.slice.call(fileInput.files);"
          "var unit = document.getElementById('dxfUnit').value;"
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
          "jobs.push({name: f.name, svgName: svgName, blob: new Blob([result.svg], {type: 'image/svg+xml'}), msg: msg});"
          "} else {"
          "jobs.push({name: f.name, svgName: f.name, blob: f});"
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
          "var conflicts = findOverwriteConflicts(toUpload.map(function(j) { return j.svgName; }));"
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
          "var resp = await fetch('/upload', {method: 'POST', body: fd});"
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
    uploadFile = FFat.open(joinFolder(base), FILE_WRITE);
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

  server.sendHeader("Location", "/");
  server.send(303);
}

// The delete form now submits one checkbox per selected file, all named
// "name" - loop over every arg with that name instead of assuming exactly
// one, so a single POST can remove any number of files at once.
static void handleDelete() {
  std::vector<String> names;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "name") {
      String base = basenameOf(server.arg(i));
      if (base.length() > 0) names.push_back(base);
    }
  }
  if (names.empty()) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  ledSet(LED_BLINK_FAST); // busy

  std::vector<String> deleted;
  std::vector<String> failed;
  if (storageBeginAppAccess()) {
    for (size_t i = 0; i < names.size(); i++) {
      if (FFat.remove(joinFolder(names[i]))) {
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
  server.sendHeader("Location", "/");
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
  server.on("/rescan", HTTP_POST, handleRescan);
  server.on("/led-toggle", HTTP_POST, handleLedToggle);
  server.on("/dxf2svg.js", HTTP_GET, handleDxfScript);
  server.on("/style.css", HTTP_GET, handleStyleCss);
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
