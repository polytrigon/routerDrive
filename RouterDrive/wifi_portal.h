// RouterDrive - Wi-Fi provisioning
//
// Boot behavior:
//  1. If credentials are saved (Preferences) and the BOOT button isn't
//     being held, try to join that network for WIFI_CONNECT_TIMEOUT_MS.
//  2. If that succeeds, we're in STATION mode - reachable at
//     http://<HOSTNAME>.local/ on your home network.
//  3. If there are no saved credentials, the connect attempt times out, or
//     BOOT was held during power-on, we start SoftAP + a captive portal at
//     http://192.168.4.1/ (SSID/password in config.h) so you can join it
//     from a phone/laptop and submit new credentials.
#pragma once

#include <Arduino.h>

enum WifiState { WIFI_STATE_AP, WIFI_STATE_STA };

// Call once from setup(). Reads saved credentials, attempts STA connect,
// falls back to AP + captive portal as described above.
void wifiPortalInit();

// Call from loop(); services the captive-portal DNS server while in AP mode
// and periodically retries a saved network if we're in AP mode because a
// connect attempt failed (rather than because no credentials exist yet).
void wifiPortalLoop();

// Current mode, so the web UI can decide what to show.
WifiState wifiPortalState();

// Save new credentials, then reboot into STA mode to try them.
void wifiPortalSaveCredentials(const String &ssid, const String &password);

// Wipe saved credentials and reboot into AP/setup mode.
void wifiPortalResetCredentials();

// Human-readable status for the web UI (e.g. "Connected to HomeNet (STA) -
// http://routerdrive.local/" or "Setup mode - join Wi-Fi 'RouterDrive-Setup'").
String wifiPortalStatusText();

// Signal strength in dBm (e.g. -54) while connected to a network (STA mode).
// Meaningless in AP/setup mode - callers should check wifiPortalState() first.
int wifiPortalRSSI();
