// RouterDrive - shared configuration constants
#pragma once

// ---- Wi-Fi setup / captive portal -----------------------------------------

// SSID and password broadcast by the ESP32 on first boot / when it can't
// join a saved network. Change PORTAL_PASSWORD before you deploy this
// somewhere other people can reach - WPA2 requires 8+ characters.
static const char *PORTAL_SSID = "RouterDrive-Setup";
static const char *PORTAL_PASSWORD = "routerdrive";

// mDNS / network hostname once connected to your home Wi-Fi.
// The device will be reachable at http://<HOSTNAME>.local/
static const char *HOSTNAME = "routerdrive";

// How long (ms) to wait for the saved Wi-Fi network before giving up and
// falling back to the setup Access Point.
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// How long (ms) to pause after bringing up USB Mass Storage before starting
// Wi-Fi. Starting the radio (esp_wifi_start(), under WiFi.begin()) right on
// top of a freshly-started native USB/TinyUSB session is a known rough edge
// on the ESP32-S3 - rarely, restarting RouterDrive while it's plugged into
// the Origin has shown the drive briefly enumerate then drop out again,
// right around when Wi-Fi would be starting up. This gives the USB
// enumeration handshake a little room to finish first. It's a mitigation
// for a timing race, not a guaranteed fix - if the device isn't plugged
// into a USB host yet, this just adds a fixed, harmless delay to every
// boot.
static const uint32_t USB_WIFI_STARTUP_SETTLE_MS = 500;

// Namespace used in NVS (via Preferences) to store Wi-Fi credentials.
static const char *PREFS_NAMESPACE = "routerdrive";

// Hold the BOOT button (GPIO0) down through power-on for this long to wipe
// saved Wi-Fi credentials and force the setup Access Point on this boot.
static const uint32_t BOOT_RESET_HOLD_MS = 3000;
#define BOOT_BUTTON_PIN 0

// ---- Storage ----------------------------------------------------------

// Where uploaded files land inside the exposed volume. Start with root ("/")
// - the Shaper Origin's exact folder requirement is not publicly documented,
// so test with a plain FAT32 flash drive first (see README) and change this
// if the Origin turns out to need a specific subfolder, e.g. "/SVGs".
static const char *SVG_FOLDER = "/";

// The block size advertised over USB. This is NOT the authority any more:
// storage.cpp asks the wear-levelling layer for the real sector size at
// boot (wl_sector_size(), the flash erase-sector size) and uses that, so
// the value here is a documented expectation and a cross-check - a
// mismatch is reported over serial rather than silently assumed away.
//
// Kept at the erase-sector size rather than the conventional 512 bytes on
// purpose: NOR flash can only erase whole sectors, so a 512-byte block
// would mean erasing 4KB to replace 512 bytes and destroying the other
// 3.5KB. Matching the two makes that impossible by construction. The cost
// is that 4K logical blocks are unusual for USB mass storage - legal, but
// not every host is happy with them.
static const uint32_t MSC_BLOCK_SIZE = 4096;

// ---- Web UI -------------------------------------------------------------

// The file list shows a search box and Prev/Next pager once there are more
// than this many files, and shows this many per page. Below this count,
// everything just shows on one page with no search box (not worth the
// clutter for a handful of files).
static const int FILES_PER_PAGE = 10;

// ---- Time (for the file list's "Uploaded" column) ------------------------

// The ESP32 has no battery-backed clock, so file upload timestamps require
// syncing time over the network - wifi_portal.cpp calls configTime() with
// these once it joins your Wi-Fi (needs internet access to actually reach
// the NTP server; if that fails, e.g. no internet or AP/setup mode only,
// uploaded files just show no date instead of a wrong one).
//
// GMT_OFFSET_SEC/DAYLIGHT_OFFSET_SEC default to plain UTC. Set them to your
// local timezone if you'd rather see local times, e.g. US Eastern is
// -18000 (standard) or -14400 (daylight); leave DAYLIGHT_OFFSET_SEC at 0
// if your area doesn't observe DST or you've already folded it into
// GMT_OFFSET_SEC.
static const char *NTP_SERVER = "pool.ntp.org";
static const long GMT_OFFSET_SEC = 0;
static const int DAYLIGHT_OFFSET_SEC = 0;
