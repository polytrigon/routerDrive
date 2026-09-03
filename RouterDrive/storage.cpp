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

static USBMSC msc;
static EspClass flashOps;
static const esp_partition_t *ffatPartition = nullptr;

enum StorageMode { MODE_USB, MODE_APP };
static volatile StorageMode currentMode = MODE_USB;
static volatile bool usbConnected = false;

// ---------------------------------------------------------------------------
// Raw block callbacks used ONLY while currentMode == MODE_USB. They talk
// straight to the flash partition, bypassing any filesystem layer, because
// the USB host (Origin/PC) is the one interpreting the FAT structure on its
// end - we just hand it bytes at a logical block address (lba).
// ---------------------------------------------------------------------------

static int32_t onUsbWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (currentMode != MODE_USB || ffatPartition == nullptr) {
    return -1;
  }
  uint32_t addr = offset + (lba * MSC_BLOCK_SIZE);
  // NOR flash can only clear bits via a full sector erase, so every write
  // has to erase first. MSC_BLOCK_SIZE matches the partition's erase-sector
  // size, so this stays sector-aligned.
  if (flashOps.partitionEraseRange(ffatPartition, addr, bufsize) != true) {
    return -1;
  }
  if (flashOps.partitionWrite(ffatPartition, addr, (uint32_t *)buffer, bufsize) != true) {
    return -1;
  }
  return bufsize;
}

static int32_t onUsbRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (currentMode != MODE_USB || ffatPartition == nullptr) {
    return -1;
  }
  uint32_t addr = offset + (lba * MSC_BLOCK_SIZE);
  if (flashOps.partitionRead(ffatPartition, addr, (uint32_t *)buffer, bufsize) != true) {
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
  currentMode = MODE_USB;

  msc.vendorID("RtrDrive");
  msc.productID("SVG Drive");
  msc.productRevision("1.0");
  msc.onRead(onUsbRead);
  msc.onWrite(onUsbWrite);
  msc.onStartStop(onUsbStartStop);
  msc.mediaPresent(true);
  msc.isWritable(true);
  msc.begin(ffatPartition->size / MSC_BLOCK_SIZE, MSC_BLOCK_SIZE);

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
  if (!FFat.begin(false, "/ffat", 10, "ffat")) {
    Serial.println("[storage] app-mode mount failed");
    currentMode = MODE_USB;
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
