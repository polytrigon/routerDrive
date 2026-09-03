#include "led.h"
#include "wifi_portal.h"
#include "config.h"

#include <Preferences.h>

// XIAO ESP32-S3's onboard user LED - active LOW.
#define LED_PIN 21

static LedMode currentMode = LED_OFF;
static bool blinkOn = false;
static uint32_t lastToggleMs = 0;
static bool ledEnabled = true;

// Gates every actual pin write on the user's on/off preference - every
// caller above (ledSet/ledLoop/ledFlashConfirm) keeps working exactly as
// before, it just stops reaching the physical LED while disabled.
static void ledWrite(bool on) {
  digitalWrite(LED_PIN, (on && ledEnabled) ? LOW : HIGH);
}

void ledInit() {
  pinMode(LED_PIN, OUTPUT);
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true /* read-only */);
  ledEnabled = prefs.getBool("ledon", true);
  prefs.end();
  ledWrite(false);
}

void ledSet(LedMode mode) {
  currentMode = mode;
  lastToggleMs = millis();
  blinkOn = true;
  switch (mode) {
    case LED_OFF:
      ledWrite(false);
      break;
    case LED_ON:
      ledWrite(true);
      break;
    case LED_BLINK_SLOW:
    case LED_BLINK_FAST:
      ledWrite(true); // start each new pattern lit, so the change is immediately visible
      break;
  }
}

void ledApplyIdleState() {
  ledSet(wifiPortalState() == WIFI_STATE_AP ? LED_BLINK_SLOW : LED_ON);
}

void ledLoop() {
  uint32_t interval;
  switch (currentMode) {
    case LED_BLINK_SLOW: interval = 600; break;
    case LED_BLINK_FAST: interval = 120; break;
    default: return; // LED_OFF / LED_ON are static - nothing to advance
  }
  uint32_t now = millis();
  if (now - lastToggleMs >= interval) {
    lastToggleMs = now;
    blinkOn = !blinkOn;
    ledWrite(blinkOn);
  }
}

void ledFlashConfirm() {
  for (int i = 0; i < 4; i++) {
    ledWrite(true);
    delay(80);
    ledWrite(false);
    delay(80);
  }
}

bool ledIsEnabled() {
  return ledEnabled;
}

void ledSetEnabled(bool enabled) {
  ledEnabled = enabled;
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBool("ledon", enabled);
  prefs.end();
  if (enabled) {
    ledApplyIdleState(); // resume showing the right pattern for the current Wi-Fi state
  } else {
    ledWrite(false); // go dark immediately, don't wait for the next ledLoop() tick
  }
}
