#include "wifi_portal.h"
#include "config.h"
#include "led.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>

static Preferences prefs;
static DNSServer dnsServer;
static WifiState state = WIFI_STATE_AP;
static bool haveSavedCreds = false;
static uint32_t lastRetryMs = 0;
static const uint32_t RETRY_INTERVAL_MS = 60000;

static bool loadCredentials(String &ssid, String &pass) {
  prefs.begin(PREFS_NAMESPACE, true /* read-only */);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

// A static IP explicitly saved via the web UI - separate from the
// config.h compile-time fallback, so callers (resolveStaticIP() below,
// and the web UI's status text) can tell the two apart.
static bool loadSavedStaticIP(String &ip, String &gateway, String &subnet, String &dns) {
  prefs.begin(PREFS_NAMESPACE, true /* read-only */);
  String mode = prefs.getString("ipmode", "");
  ip = prefs.getString("ip", "");
  gateway = prefs.getString("gw", "");
  subnet = prefs.getString("sn", "");
  dns = prefs.getString("dns", "");
  prefs.end();
  return mode == "static" && ip.length() > 0;
}

// True only if the web UI's "Use DHCP instead" was explicitly clicked -
// distinct from simply never having saved a static IP, so an explicit
// choice here always wins over config.h's USE_STATIC_IP_FALLBACK.
static bool explicitlyWantsDHCP() {
  prefs.begin(PREFS_NAMESPACE, true /* read-only */);
  String mode = prefs.getString("ipmode", "");
  prefs.end();
  return mode == "dhcp";
}

// Resolves the static IP actually in effect, if any: a web-UI-saved value
// takes priority; otherwise config.h's USE_STATIC_IP_FALLBACK (unless the
// web UI explicitly chose DHCP instead); otherwise none (plain DHCP).
// Returns false for plain DHCP. fromFallback, if given, reports which
// source won - purely for the web UI's status text, doesn't affect
// behavior.
static bool resolveStaticIP(String &ip, String &gateway, String &subnet, String &dns, bool *fromFallback = nullptr) {
  if (loadSavedStaticIP(ip, gateway, subnet, dns)) {
    if (fromFallback) *fromFallback = false;
    return true;
  }
  if (!explicitlyWantsDHCP() && USE_STATIC_IP_FALLBACK) {
    ip = STATIC_IP_FALLBACK;
    gateway = STATIC_GATEWAY_FALLBACK;
    subnet = STATIC_SUBNET_FALLBACK;
    dns = STATIC_DNS_FALLBACK;
    if (fromFallback) *fromFallback = true;
    return true;
  }
  return false;
}

static void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PORTAL_SSID, PORTAL_PASSWORD);
  delay(100);
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);
  state = WIFI_STATE_AP;
  Serial.printf("[wifi] AP mode: join \"%s\" then visit http://%s/\n", PORTAL_SSID, apIP.toString().c_str());

  // Also start mDNS here, not just after a STA connect (see connectSTA()) -
  // this is what lets the AP-mode streamlined page's "/nowifi" escape
  // hatch point at the friendlier "routerdrive.local/nowifi" instead of a
  // raw IP. Works the same way over the ESP32's own SoftAP subnet as it
  // does on a home network; Apple devices (the ones this is mainly for)
  // resolve it especially reliably. The AP IP is still offered as a
  // fallback in the UI copy in case some client's mDNS resolution doesn't
  // cooperate.
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[wifi] also reachable at http://%s.local/ (AP mode)\n", HOSTNAME);
  } else {
    Serial.println("[wifi] mDNS responder failed to start in AP mode (IP above still works)");
  }
}

static bool connectSTA(const String &ssid, const String &pass, bool keepAPAlive = false) {
  Serial.printf("[wifi] joining \"%s\"...\n", ssid.c_str());
  // keepAPAlive is set for the periodic retry in wifiPortalLoop(), where
  // we're already running the setup AP and must not drop it just to test a
  // saved network - see the comment above that call for why.
  WiFi.mode(keepAPAlive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(HOSTNAME);

  // Static IP, if one's configured (web-UI-saved, or config.h's fallback -
  // see resolveStaticIP()) - must happen after WiFi.mode() and before
  // WiFi.begin(), or the interface config doesn't stick.
  String ip, gateway, subnet, dns;
  if (resolveStaticIP(ip, gateway, subnet, dns)) {
    IPAddress ipAddr, gwAddr, snAddr, dnsAddr;
    ipAddr.fromString(ip);
    gwAddr.fromString(gateway);
    snAddr.fromString(subnet.length() > 0 ? subnet : "255.255.255.0");
    dnsAddr.fromString(dns.length() > 0 ? dns : gateway);
    if (WiFi.config(ipAddr, gwAddr, snAddr, dnsAddr)) {
      Serial.printf("[wifi] using static IP %s\n", ip.c_str());
    } else {
      Serial.println("[wifi] WiFi.config() for static IP failed - falling back to DHCP");
    }
  }

  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    // ledDelay(), not delay(): on the first call this runs from setup(),
    // where loop() hasn't started and nothing else is advancing the boot
    // blink. This is the longest single stretch of boot - up to
    // WIFI_CONNECT_TIMEOUT_MS - so it's the one that most needs the LED to
    // keep showing signs of life.
    ledDelay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] connect failed / timed out");
    if (keepAPAlive) {
      // Drop back to a plain AP rather than leaving the radio sitting in
      // AP_STA with a dead, disconnected STA side for no reason.
      WiFi.mode(WIFI_AP);
    }
    return false;
  }

  Serial.printf("[wifi] connected, IP: %s\n", WiFi.localIP().toString().c_str());

  // Kicks off an async NTP sync - doesn't block here, but until it lands
  // (needs internet access, not just a local network) file timestamps
  // (FFat's File::getLastWrite(), used for the web UI's "Uploaded" column)
  // won't be meaningful yet. See config.h for the timezone constants.
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  if (keepAPAlive) {
    // A retry from AP mode just succeeded - we're switching over to STA for
    // good, so drop the setup AP now that there's a working connection.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[wifi] reachable at http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("[wifi] mDNS responder failed to start (IP above still works)");
  }
  state = WIFI_STATE_STA;
  return true;
}

static bool bootResetRequested() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BUTTON_PIN) != LOW) {
    return false; // not held
  }
  Serial.println("[wifi] BOOT button held - keep holding to reset saved Wi-Fi...");
  uint32_t start = millis();
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (millis() - start >= BOOT_RESET_HOLD_MS) {
      return true;
    }
    delay(20);
  }
  return false;
}

void wifiPortalInit() {
  if (bootResetRequested()) {
    Serial.println("[wifi] BOOT hold confirmed - clearing saved credentials");
    prefs.begin(PREFS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
  }

  String ssid, pass;
  haveSavedCreds = loadCredentials(ssid, pass);

  if (haveSavedCreds && connectSTA(ssid, pass)) {
    return;
  }
  startAP();
}

void wifiPortalLoop() {
  if (state == WIFI_STATE_AP) {
    dnsServer.processNextRequest();
    if (haveSavedCreds && (millis() - lastRetryMs) > RETRY_INTERVAL_MS) {
      lastRetryMs = millis();
      String ssid, pass;
      // true = keep the setup AP broadcasting during this attempt. Without
      // it, a bad/out-of-range saved network would silently tear down
      // "RouterDrive-Setup" every retry and never bring it back, leaving no
      // way back into the portal short of a reflash or a BOOT-button reset.
      if (loadCredentials(ssid, pass) && connectSTA(ssid, pass, true)) {
        dnsServer.stop();
        ledApplyIdleState(); // was blinking for AP/setup mode - reflect the new STA state
      }
    }
  }
}

WifiState wifiPortalState() {
  return state;
}

void wifiPortalSaveCredentials(const String &ssid, const String &password) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
  Serial.println("[wifi] credentials saved, rebooting...");
  delay(500);
  ESP.restart();
}

void wifiPortalResetCredentials() {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  Serial.println("[wifi] credentials cleared, rebooting...");
  delay(300);
  ESP.restart();
}

String wifiPortalStatusText() {
  if (state == WIFI_STATE_STA) {
    String s = "Connected to \"" + WiFi.SSID() + "\" - http://";
    s += HOSTNAME;
    s += ".local/  (or http://";
    s += WiFi.localIP().toString();
    s += "/)";
    return s;
  }
  String s = "Setup mode - join Wi-Fi \"";
  s += PORTAL_SSID;
  s += "\", then visit http://";
  s += WiFi.softAPIP().toString();
  s += "/";
  return s;
}

int wifiPortalRSSI() {
  return WiFi.RSSI();
}

void wifiPortalSaveStaticIP(const String &ip, const String &gateway, const String &subnet, const String &dns) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ipmode", "static");
  prefs.putString("ip", ip);
  prefs.putString("gw", gateway);
  prefs.putString("sn", subnet);
  prefs.putString("dns", dns);
  prefs.end();
  Serial.println("[wifi] static IP saved, rebooting...");
  delay(500);
  ESP.restart();
}

void wifiPortalClearStaticIP() {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ipmode", "dhcp"); // explicit - overrides config.h's fallback too
  prefs.remove("ip");
  prefs.remove("gw");
  prefs.remove("sn");
  prefs.remove("dns");
  prefs.end();
  Serial.println("[wifi] static IP cleared, back to DHCP, rebooting...");
  delay(300);
  ESP.restart();
}

String wifiPortalStaticIPStatusText() {
  String ip, gateway, subnet, dns;
  bool fromFallback = false;
  if (resolveStaticIP(ip, gateway, subnet, dns, &fromFallback)) {
    return fromFallback ? ("Static: " + ip + " (from config.h fallback)") : ("Static: " + ip);
  }
  return "Automatic (DHCP)";
}
