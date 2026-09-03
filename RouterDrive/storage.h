// RouterDrive - storage / USB Mass Storage management
//
// The hard constraint (confirmed by Espressif's own tusb_msc example and by
// community reports): the ESP32-S3's internal flash can be mounted as a
// normal filesystem for the firmware to read/write ("app mode"), OR handed
// to the USB host as raw sectors ("USB mode") - never safely both at once.
// So instead of true concurrency, RouterDrive *switches* between the two,
// automatically, around every write the web UI makes. The switch is quick
// (well under a second for a small SVG) and, unless the Shaper Origin
// happens to be reading a file in that exact instant, is invisible to it.
//
// After every switch back to USB mode we also nudge the USB host to notice
// the change (see usbSoftRescan()) so you shouldn't need to unplug the
// cable to see newly-uploaded files.
#pragma once

#include <Arduino.h>

// Call once from setup(), after Serial.begin(). Mounts/formats the FAT
// partition, sets up the USBMSC callbacks, and leaves the device in USB
// mode (host-visible) by default.
void storageInit();

// Call from loop(); currently a no-op placeholder kept for symmetry with
// wifiPortalLoop()/webServerLoop() in case you add polling logic later.
void storageLoop();

// Switch storage over to the firmware so it can use normal file calls
// (FFat.open, FFat.exists, listing directories, etc). Safe to call again
// if already in app mode. Returns false if the mount failed.
bool storageBeginAppAccess();

// Hand storage back to the USB host and (by default) trigger a rescan so
// newly written/deleted files show up without a physical replug.
void storageEndAppAccess(bool rescan = true);

// Ask the connected USB host to re-read the filesystem via the "soft"
// SCSI media-change hint (report NOT READY, then READY again). Cheap and
// instant, but whether a given host (including the Origin) honors it is
// unverified - test it on your hardware.
void usbSoftRescan();

// Fallback if usbSoftRescan() doesn't get the Origin to notice: restarts
// the whole device, which forces a genuine USB disconnect/reconnect (the
// arduino-esp32 core has no way to bounce just the USB peripheral on its
// own). Takes several seconds, drops Wi-Fi along with everything else, and
// will interrupt the Origin if it's actively reading over USB at that
// moment - so this is a manual "last resort" button, not something to run
// after every upload. IMPORTANT: send any HTTP response to the caller
// BEFORE calling this; the device goes away immediately.
void usbHardReconnect();

// True if storage is currently mounted for firmware use (app mode) rather
// than exposed to the USB host.
bool storageInAppMode();

// True once the native USB peripheral has seen a host enumerate it (i.e. a
// cable is plugged into a powered-on USB port, PC or Origin alike), false
// after it's unplugged. Just confirms the physical/electrical link is up -
// it can't tell you whether the Origin specifically has indexed the drive,
// since that's internal to Origin's own (closed) software.
bool usbIsConnected();
