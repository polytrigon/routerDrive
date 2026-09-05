// RouterDrive - onboard user LED status indicator
//
// The XIAO ESP32-S3 has a single user LED wired to GPIO21, active-LOW (the
// pin must be driven LOW to turn it on, HIGH to turn it off) - separate
// from the tiny hardware-only charging-status LED, which isn't software
// controllable. Source: Seeed's own "Getting Started" wiki page plus the
// XIAO ESP32S3 community forum (both point at GPIO21, active-low).
//
// First pass: a small, fixed set of states covering the situations that
// are actually worth a glance-able signal for. All blinking is
// non-blocking (advanced from ledLoop(), called every pass of loop()) -
// the one exception is ledFlashConfirm(), which blocks on purpose since
// it's only ever called right before a restart anyway.
#pragma once

#include <Arduino.h>

enum LedMode {
  LED_OFF,         // not currently assigned to anything, kept for completeness
  LED_ON,          // solid - connected to Wi-Fi and idle, ready to use
  LED_BLINK_SLOW,  // Wi-Fi setup/AP mode - needs attention
  LED_BLINK_FAST,  // busy - uploading or deleting a file right now
};

// Call once from setup(), before anything that might call ledSet()/
// ledApplyIdleState() - i.e. before wifiPortalInit().
void ledInit();

// Set the LED to a specific mode directly.
void ledSet(LedMode mode);

// Convenience: sets the LED to whatever "idle" should look like for the
// current Wi-Fi state (LED_BLINK_SLOW in AP/setup mode, LED_ON once
// connected). Call this after any transient state wraps up (an
// upload/delete finishes, Wi-Fi just connected) to fall back to reflecting
// steady-state.
void ledApplyIdleState();

// Call from loop() every pass; advances whatever blink pattern is active.
// Cheap and non-blocking - safe to call unconditionally every loop().
void ledLoop();

// A drop-in replacement for delay() that keeps the current blink pattern
// advancing while it waits. Use it anywhere setup() blocks noticeably -
// loop() (and therefore ledLoop()) isn't running yet at that point, so a
// plain delay() would leave the LED frozen mid-pattern.
void ledDelay(uint32_t ms);

// Blocking confirmation flash (a handful of quick on/off cycles), used
// right before a restart so the button press has a visible "yes, this is
// happening" moment even though the actual restart takes a few seconds.
void ledFlashConfirm();

// User preference, persisted in NVS (Preferences, same namespace as Wi-Fi
// credentials) so it survives reboots: whether the LED is allowed to light
// up at all. Every ledSet()/ledLoop() call above still tracks state
// internally either way - this only gates the actual physical output, so
// re-enabling immediately resumes showing the correct pattern with no
// extra bookkeeping. Exposed via a small toggle in the web UI's footer for
// anyone who'd rather it just stay dark.
bool ledIsEnabled();
void ledSetEnabled(bool enabled);
