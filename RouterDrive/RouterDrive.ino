// RouterDrive
// Wireless SVG "USB drive" for the Shaper Origin, built on a Seeed XIAO
// ESP32-S3. See README.md for setup instructions before building this.
//
// - storage.*     USB Mass Storage (exposes onboard flash to the Origin)
// - wifi_portal.* First-boot Wi-Fi setup (Access Point + captive portal)
//                 and reconnecting to a saved network afterwards.
// - web_server.*  The browser UI: upload SVGs, manage files, edit Wi-Fi.
#include "config.h"
#include "storage.h"
#include "wifi_portal.h"
#include "web_server.h"
#include "led.h"

void setup() {
  Serial.begin(115200);
  delay(300); // let USB-CDC serial settle before we start printing
  Serial.println();
  Serial.println("=== RouterDrive starting ===");
  Serial.println("Built " __DATE__ " " __TIME__); // same build stamp shown in the web UI's footer

  ledInit();

  // Bring up storage / USB MSC first so the Origin sees the drive as soon
  // as possible, independent of however long Wi-Fi takes to connect.
  storageInit();

  // Brief pause before touching Wi-Fi - see USB_WIFI_STARTUP_SETTLE_MS in
  // config.h for why (a rare USB-enumeration-vs-radio-startup race on the
  // ESP32-S3).
  delay(USB_WIFI_STARTUP_SETTLE_MS);

  wifiPortalInit();
  ledApplyIdleState(); // reflect whichever mode wifiPortalInit() landed in (AP or STA)
  webServerInit();

  Serial.println("=== RouterDrive ready ===");
}

void loop() {
  wifiPortalLoop();
  webServerLoop();
  storageLoop();
  ledLoop();
}
