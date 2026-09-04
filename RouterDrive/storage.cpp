#include "storage.h"
#include "config.h"

#include <Arduino.h>
#include <string.h>

#if !SOC_USB_OTG_SUPPORTED
#error This board has no native USB-OTG peripheral - wrong board selected?
#elif ARDUINO_USB_MODE
#error Set Tools > USB Mode to "USB-OTG (TinyUSB)", not "Hardware CDC and JTAG"
#endif

#include "USB.h"
#include "USBMSC.h"
#include "FFat.h"
#include "FS.h"
#include "esp_partition.h"
#include "wear_levelling.h"

static USBMSC msc;
static const esp_partition_t *ffatPartition = nullptr;

// ---------------------------------------------------------------------------
// Wear levelling, and why the USB side has to go through it too.
//
// FFat.begin() does NOT put the filesystem straight on the partition - it
// mounts through Espressif's wear-levelling (WL) layer
// (esp_vfs_fat_spiflash_mount_rw_wl(), which is why FFat holds a
// wl_handle_t). WL deliberately remaps logical sectors onto different
// physical ones so no single sector takes every erase - FAT hammers the
// same few sectors (the allocation table, the root directory) on every
// write, and without WL those wear out first - and it keeps its own
// bookkeeping inside the partition.
//
// This used to read and write the partition RAW here, via
// esp_partition_read/write at lba * sector_size, bypassing WL entirely.
// That meant the two sides addressed the same flash through two different
// translations: the app saw the filesystem through WL, the USB host saw
// physical bytes. They agree while the partition is lightly written -
// which is why it worked - but WL's mapping migrates as erase cycles
// accumulate, so the host's view would eventually slide out from under
// the filesystem. A host write would also have landed behind WL's back,
// leaving its bookkeeping inconsistent with the data.
//
// Both sides now go through WL, so there is only ONE translation and the
// two views agree by construction. Only one WL instance is ever live: the
// handle below is mounted while we're in MODE_USB and unmounted before
// FFat mounts its own in MODE_APP (see storageBegin/EndAppAccess). WL's
// state lives in the partition, not in the instance, so a fresh mount
// picks up exactly the mapping the other side left behind.
// ---------------------------------------------------------------------------
static wl_handle_t usbWlHandle = WL_INVALID_HANDLE;
// Geometry as WL reports it, read once at boot. usbSectorSize is WL's
// sector size (the flash erase-sector size, 4KB) and usbBlockCount covers
// only the usable area - wl_size() already excludes WL's own overhead,
// unlike the raw partition size this used to report.
static size_t usbSectorSize = 0;
static size_t usbBlockCount = 0;

enum StorageMode { MODE_USB, MODE_APP };
static volatile StorageMode currentMode = MODE_USB;
static volatile bool usbConnected = false;

// ---------------------------------------------------------------------------
// Block callbacks used ONLY while currentMode == MODE_USB. The USB host
// (Origin/PC) is the one interpreting the FAT structure on its end - we
// just hand it bytes at a logical block address (lba), through the same
// wear-levelling translation the filesystem side uses.
// ---------------------------------------------------------------------------

static int32_t onUsbWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (currentMode != MODE_USB || usbWlHandle == WL_INVALID_HANDLE || usbSectorSize == 0) {
    return -1;
  }
  size_t addr = (size_t)lba * usbSectorSize + offset;
  // NOR flash can only clear bits via a full sector erase, so every write
  // has to erase first - and wl_erase_range() requires both the address
  // and the length to be whole sectors. A partial-sector write would
  // erase past what it is replacing and take the neighbouring data with
  // it, so refuse it instead: a rejected write is a host-visible error,
  // a silent one is lost files. (Nothing should hit this - MSC transfers
  // whole blocks and the Origin only ever reads - but the cost of being
  // wrong here is other people's work.)
  if ((addr % usbSectorSize) != 0 || (bufsize % usbSectorSize) != 0) {
    Serial.printf("[storage] refused unaligned USB write: addr=%u size=%u sector=%u\n",
                  (unsigned)addr, (unsigned)bufsize, (unsigned)usbSectorSize);
    return -1;
  }
  if (wl_erase_range(usbWlHandle, addr, bufsize) != ESP_OK) {
    return -1;
  }
  if (wl_write(usbWlHandle, addr, buffer, bufsize) != ESP_OK) {
    return -1;
  }
  return bufsize;
}

static int32_t onUsbRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (currentMode != MODE_USB || usbWlHandle == WL_INVALID_HANDLE || usbSectorSize == 0) {
    return -1;
  }
  size_t addr = (size_t)lba * usbSectorSize + offset;
  if (wl_read(usbWlHandle, addr, buffer, bufsize) != ESP_OK) {
    return -1;
  }
  return bufsize;
}

static bool onUsbStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  (void)start;
  (void)load_eject;
  return true;
}

static void usbEventCallback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_data;
  if (event_base != ARDUINO_USB_EVENTS) {
    return;
  }
  switch (event_id) {
    case ARDUINO_USB_STARTED_EVENT:
      usbConnected = true;
      Serial.println("[usb] plugged in / enumerated");
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      usbConnected = false;
      Serial.println("[usb] unplugged");
      break;
    default: break;
  }
}

static void ensureSvgFolderExists() {
  if (strcmp(SVG_FOLDER, "/") == 0) {
    return;
  }
  if (!FFat.exists(SVG_FOLDER)) {
    FFat.mkdir(SVG_FOLDER);
  }
}

void storageInit() {
  ffatPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (ffatPartition == nullptr) {
    Serial.println("[storage] FATAL: no 'ffat' data partition found.");
    Serial.println("[storage] Did you select Tools > Partition Scheme > Custom");
    Serial.println("[storage] with partitions.csv present in the sketch folder?");
    return;
  }

  // Mount once at boot (app mode) so we can format-on-first-use and make
  // sure the SVG folder exists, then hand off to the USB host.
  Serial.println("[storage] mounting FAT filesystem...");
  if (!FFat.begin(false, "/ffat", 10, "ffat")) {
    Serial.println("[storage] mount failed, formatting...");
    if (!FFat.format(FFAT_WIPE_FULL)) {
      Serial.println("[storage] FATAL: format failed.");
      return;
    }
    if (!FFat.begin(false, "/ffat", 10, "ffat")) {
      Serial.println("[storage] FATAL: mount failed even after formatting.");
      return;
    }
  }
  ensureSvgFolderExists();
  Serial.printf("[storage] %lu / %lu bytes used\n", (unsigned long)FFat.usedBytes(), (unsigned long)FFat.totalBytes());
  FFat.end();

  // Hand the flash to the USB side by mounting our own WL instance now
  // that FFat has released its one. This also tells us the geometry to
  // advertise: wl_size() is the usable area, already excluding WL's own
  // overhead. (This used to advertise ffatPartition->size / 4KB, i.e. the
  // whole raw partition - more blocks than the filesystem actually spans,
  // with WL's bookkeeping at the end exposed to the host as if it were
  // data.)
  if (wl_mount(ffatPartition, &usbWlHandle) != ESP_OK) {
    usbWlHandle = WL_INVALID_HANDLE;
    Serial.println("[storage] FATAL: could not mount wear levelling for USB access.");
    return;
  }
  usbSectorSize = wl_sector_size(usbWlHandle);
  usbBlockCount = usbSectorSize > 0 ? (wl_size(usbWlHandle) / usbSectorSize) : 0;
  if (usbSectorSize == 0 || usbBlockCount == 0) {
    Serial.println("[storage] FATAL: wear levelling reported an unusable geometry.");
    wl_unmount(usbWlHandle);
    usbWlHandle = WL_INVALID_HANDLE;
    return;
  }
  // MSC_BLOCK_SIZE is the value the rest of the project assumes; if WL
  // ever disagrees, the runtime value wins (it's the one that's actually
  // correct) but say so loudly, because it means a config assumption has
  // drifted from the hardware.
  if (usbSectorSize != MSC_BLOCK_SIZE) {
    Serial.printf("[storage] NOTE: WL sector size is %u, config.h expects %u - using WL's.\n",
                  (unsigned)usbSectorSize, (unsigned)MSC_BLOCK_SIZE);
  }
  Serial.printf("[storage] exposing %u blocks of %u bytes (%lu bytes usable)\n",
                (unsigned)usbBlockCount, (unsigned)usbSectorSize,
                (unsigned long)wl_size(usbWlHandle));
  currentMode = MODE_USB;

  msc.vendorID("RtrDrive");
  msc.productID("SVG Drive");
  msc.productRevision("1.0");
  msc.onRead(onUsbRead);
  msc.onWrite(onUsbWrite);
  msc.onStartStop(onUsbStartStop);
  msc.mediaPresent(true);
  msc.isWritable(true);
  msc.begin(usbBlockCount, usbSectorSize);

  USB.onEvent(usbEventCallback);
  USB.begin();

  Serial.println("[storage] ready, exposed to USB host.");
}

void storageLoop() {
  // Nothing to poll yet - kept for symmetry with the other modules.
}

bool storageBeginAppAccess() {
  if (currentMode == MODE_APP) {
    return true;
  }
  if (ffatPartition == nullptr) {
    return false;
  }
  // Tell the host "no media" *before* we touch the flash, so any read/write
  // it has in flight gets rejected instead of racing our own access.
  msc.mediaPresent(false);
  currentMode = MODE_APP;
  // Release our WL instance before FFat mounts its own. Only one may be
  // live at a time: each keeps its mapping cached in RAM and writes its
  // own state back to the partition, so two instances over one partition
  // would overwrite each other's bookkeeping.
  if (usbWlHandle != WL_INVALID_HANDLE) {
    wl_unmount(usbWlHandle);
    usbWlHandle = WL_INVALID_HANDLE;
  }
  if (!FFat.begin(false, "/ffat", 10, "ffat")) {
    Serial.println("[storage] app-mode mount failed");
    currentMode = MODE_USB;
    // Put the USB side back the way it was, or the drive stays dark until
    // the next successful app-mode round trip.
    if (wl_mount(ffatPartition, &usbWlHandle) != ESP_OK) {
      usbWlHandle = WL_INVALID_HANDLE;
      Serial.println("[storage] and could not remount WL for USB - drive offline");
      return false;
    }
    msc.mediaPresent(true);
    return false;
  }
  ensureSvgFolderExists();
  return true;
}

void storageEndAppAccess(bool rescan) {
  if (currentMode != MODE_APP) {
    return;
  }
  FFat.end();
  currentMode = MODE_USB;
  // Take the flash back for the USB side. WL's state lives in the
  // partition, so this picks up whatever mapping FFat's instance left -
  // the host and the filesystem stay in step.
  if (wl_mount(ffatPartition, &usbWlHandle) != ESP_OK) {
    usbWlHandle = WL_INVALID_HANDLE;
    // Deliberately leave media absent: with no handle the callbacks can
    // only fail, and a drive that reports "no media" is far better than
    // one that answers reads with garbage.
    Serial.println("[storage] WL remount failed - leaving USB media absent");
    return;
  }
  msc.mediaPresent(true);
  if (rescan) {
    usbSoftRescan();
  }
}

void usbSoftRescan() {
  // mediaPresent(false) -> true above already performs the "media changed"
  // hint (NOT READY, then READY again on the next Test Unit Ready poll).
  // This function exists as an explicit, callable step for the web UI's
  // "nudge Origin to rescan" button, in case a caller wants to trigger it
  // without also flipping storage modes.
  msc.mediaPresent(false);
  delay(50);
  msc.mediaPresent(true);
}

void usbHardReconnect() {
  // arduino-esp32's ESPUSB class has no end()/re-begin() - once the USB
  // peripheral is started in setup() there's no supported way to bounce
  // just it from a running sketch. A full device restart does force a
  // genuine USB disconnect/reconnect (the peripheral physically resets
  // along with the rest of the chip), so that's the real "hard" option.
  // Caller is responsible for responding to any in-flight HTTP request
  // BEFORE calling this - everything, Wi-Fi included, drops immediately.
  Serial.println("[storage] restarting device to force a USB reconnect...");
  delay(200);
  ESP.restart();
}

bool storageInAppMode() {
  return currentMode == MODE_APP;
}

bool usbIsConnected() {
  return usbConnected;
}
