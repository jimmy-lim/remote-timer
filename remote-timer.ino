#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

constexpr uint8_t LED_PIN = 8;
unsigned long lastLedToggle = 0;
bool ledState = false;
constexpr uint8_t RIGHT_BOARD_BUTTON_PIN = 9; // On-board right button (BOOT on most ESP32-C3 boards)

// Buzzer output pin (adjust for your ESP32-C3 wiring).
constexpr uint8_t BUZZER_PIN = 10;
constexpr uint16_t SHORT_PRESS_TONES[4] = {523, 587, 659, 698}; // Do Re Mi Fa (C5 D5 E5 F5)
constexpr uint16_t LONG_PRESS_TONES[4] = {784, 880, 988, 1047}; // So La Ti Do (G5 A5 B5 C6)

// YK04 receiver outputs (adjust pins to match your board wiring).
constexpr uint8_t BUTTON_PINS[4] = {0, 1, 3, 4};
constexpr uint8_t BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
namespace TimingCfg {
constexpr unsigned long ToneMs = 90UL;
constexpr unsigned long ToneGapMs = 70UL;
constexpr unsigned long ButtonDebounceMs = 100UL;
constexpr unsigned long ButtonLongPressMs = 3000UL;
constexpr unsigned long ButtonPressCooldownMs = 1000UL;
constexpr unsigned long DeviceStatePollMs = 60000UL;
constexpr unsigned long AlertToneMs = 400UL;
constexpr unsigned long AlertToneGapMs = 100UL;
constexpr uint8_t AlertToneBeeps = 2;
constexpr unsigned long LoadedTimerToneStartDelayMs = 400UL;
constexpr unsigned long LoadedTimerToneSpacingMs = 280UL;
constexpr unsigned long LedToggleMs = 1000UL;
constexpr unsigned long LoopDelayMs = 5UL;
constexpr unsigned long BoardButtonDebounceMs = 40UL;
constexpr unsigned long BoardButtonCooldownMs = 500UL;
}

namespace WiFiCfg {
constexpr unsigned long DiagIntervalMs = 10000UL;
constexpr unsigned long RecoveryBackoffMs = 3000UL;
constexpr unsigned long ApAutoDisableDelayMs = 10000UL;
}

namespace NetCfg {
constexpr unsigned long LockTimeoutMs = 1500UL;
constexpr uint16_t HttpTimeoutMs = 1500U;
}

namespace LimitsCfg {
constexpr size_t SsidMaxLen = 32;
constexpr size_t PassMaxLen = 64;
constexpr size_t HostMaxLen = 128;
constexpr size_t TokenMaxLen = 128;
constexpr uint8_t ActionQueueLen = 4;
constexpr uint8_t ToneQueueLen = 16;
}

struct DeviceConfig {
  char ssid[LimitsCfg::SsidMaxLen + 1] = {};
  char password[LimitsCfg::PassMaxLen + 1] = {};
  char apiHost[LimitsCfg::HostMaxLen + 1] = "192.168.1.101:30109";
  char bearerToken[LimitsCfg::TokenMaxLen + 1] = {};
};

struct ButtonState {
  bool stablePressed = false;
  bool lastRawPressed = false;
  bool longTriggered = false;
  unsigned long rawChangedAt = 0;
  unsigned long pressedAt = 0;
  unsigned long cooldownUntil = 0;
};

struct TimerAction {
  uint8_t buttonIndex = 0;
  bool isDelete = false;
};

struct TonePattern {
  uint16_t frequency = 0;
  uint8_t beepsRemaining = 0;
  unsigned long toneMs = 0;
  unsigned long gapMs = 0;
};

Preferences prefs;
DeviceConfig cfg;
WebServer configServer(80);
ButtonState buttons[BUTTON_COUNT];
QueueHandle_t actionQueue = nullptr;
QueueHandle_t toneQueue = nullptr;
SemaphoreHandle_t statusMutex = nullptr;
SemaphoreHandle_t networkMutex = nullptr;

String statusLine = "Booting";

String apSsid;
String webhookBase;
String deviceStateUrl;

String suffix;
bool toneOutputActive = false;
bool tonePatternActive = false;
TonePattern currentTonePattern;
unsigned long toneStageEndsAt = 0;
unsigned long lastDeviceStateFetchAttempt = 0;
int32_t lastAlertIndex[BUTTON_COUNT] = {-1, -1, -1, -1};
unsigned long alertAfterMsByButton[BUTTON_COUNT] = {0, 0, 0, 0};
unsigned long alertIntervalMsByButton[BUTTON_COUNT] = {0, 0, 0, 0};
bool alertMutedByButton[BUTTON_COUNT] = {false, false, false, false};
bool timerActiveByButton[BUTTON_COUNT] = {false, false, false, false};
uint64_t timerElapsedBaseMsByButton[BUTTON_COUNT] = {0, 0, 0, 0};
unsigned long timerElapsedBaseAtMsByButton[BUTTON_COUNT] = {0, 0, 0, 0};
uint64_t timerEpochMsByButton[BUTTON_COUNT] = {0, 0, 0, 0};
uint32_t actionToneEnqueueCountByButton[BUTTON_COUNT] = {0, 0, 0, 0};
uint32_t alertToneEnqueueCountByButton[BUTTON_COUNT] = {0, 0, 0, 0};
bool deviceStateLoaded = false;
uint8_t loadedTimerTonePendingMask = 0;
bool loadedTimerToneStarted = false;
unsigned long loadedTimerToneNextAt = 0;
bool loadedTimerTonesPlayedThisBoot = false;
bool rebootRequested = false;
unsigned long rebootAt = 0;
wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
unsigned long lastWiFiDiagAt = 0;
int lastWiFiRssi = 0;
int lastWiFiChannel = 0;
int lastWiFiDisconnectReason = 0;
bool configApEnabled = false;
bool apDisablePending = false;
unsigned long apDisableAt = 0;
bool configPortalStarted = false;
bool boardBtnStablePressed = false;
bool boardBtnLastRawPressed = false;
unsigned long boardBtnRawChangedAt = 0;
unsigned long boardBtnCooldownUntil = 0;
bool wifiPasswordShownAtBoot = false;
uint8_t wifiAuthExpireStreak = 0;
bool wifiRecoveryPending = false;
uint8_t wifiRecoveryStage = 0;
unsigned long wifiRecoveryAt = 0;
bool startupWaitForWiFi = true;

void startPortal();
void ensureConfigServerStarted();

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "WL_UNKNOWN";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      lastWiFiChannel = static_cast<int>(info.wifi_sta_connected.channel);
      lastWiFiDisconnectReason = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      apDisablePending = false;
      lastWiFiDisconnectReason = static_cast<int>(info.wifi_sta_disconnected.reason);
      if (lastWiFiDisconnectReason == 2) {
        if (wifiAuthExpireStreak < 255) {
          wifiAuthExpireStreak++;
        }
        if (!wifiRecoveryPending && wifiAuthExpireStreak >= 8) {
          wifiRecoveryPending = true;
          wifiRecoveryStage = 1;
          wifiRecoveryAt = millis() + 50UL;
          setStatus("WiFi auth timeout, forcing clean reconnect");
          break;
        }
      } else {
        wifiAuthExpireStreak = 0;
      }
      {
        char msg[72];
        snprintf(
          msg,
          sizeof(msg),
          "WiFi disconnected (auto reconnect, reason=%d)",
          lastWiFiDisconnectReason
        );
        setStatus(msg);
      }
      // Avoid calling WiFi APIs from event context; sampled in loop diagnostics.
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      if (configApEnabled) {
        apDisablePending = true;
        apDisableAt = millis() + WiFiCfg::ApAutoDisableDelayMs;
      }
      lastWiFiDisconnectReason = 0;
      wifiAuthExpireStreak = 0;
      startupWaitForWiFi = false;
      setStatus("WiFi connected");
      break;
    }
    default:
      break;
  }
}

void updateWiFiDiagnostics(unsigned long now) {
  const wl_status_t status = WiFi.status();
  if (status != lastWiFiStatus) {
    lastWiFiStatus = status;
  }
  if (status == WL_CONNECTED && (lastWiFiDiagAt == 0 || now - lastWiFiDiagAt >= WiFiCfg::DiagIntervalMs)) {
    lastWiFiDiagAt = now;
    lastWiFiRssi = WiFi.RSSI();
    lastWiFiChannel = static_cast<int>(WiFi.channel());
  }
}

static unsigned long effectiveElapsedMs(uint8_t idx, unsigned long now) {
  if (idx >= BUTTON_COUNT || !timerActiveByButton[idx]) {
    return 0;
  }
  const long deltaSigned = static_cast<long>(now - timerElapsedBaseAtMsByButton[idx]);
  const unsigned long deltaMs = deltaSigned < 0 ? 0UL : static_cast<unsigned long>(deltaSigned);
  const uint64_t elapsed64 = timerElapsedBaseMsByButton[idx] + static_cast<uint64_t>(deltaMs);
  return elapsed64 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<unsigned long>(elapsed64);
}

static int32_t alertIndexForElapsed(unsigned long elapsedMs, unsigned long alertAfterMs, unsigned long alertIntervalMs) {
  if (alertAfterMs == 0 || elapsedMs < alertAfterMs) {
    return -1;
  }
  if (alertIntervalMs == 0) {
    return 0;
  }
  return static_cast<int32_t>((elapsedMs - alertAfterMs) / alertIntervalMs);
}

static unsigned long nextBeepElapsedMs(unsigned long elapsedMs, unsigned long alertAfterMs, unsigned long alertIntervalMs) {
  if (alertAfterMs == 0) {
    return 0;
  }
  if (elapsedMs < alertAfterMs) {
    return alertAfterMs;
  }
  if (alertIntervalMs == 0) {
    return 0;
  }

  const unsigned long intervalsElapsed = (elapsedMs - alertAfterMs) / alertIntervalMs;
  const uint64_t nextBeepMs64 = static_cast<uint64_t>(alertAfterMs)
    + static_cast<uint64_t>(intervalsElapsed + 1UL) * static_cast<uint64_t>(alertIntervalMs);
  return nextBeepMs64 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<unsigned long>(nextBeepMs64);
}

static unsigned long nextBeepInMs(unsigned long elapsedMs, unsigned long alertAfterMs, unsigned long alertIntervalMs) {
  const unsigned long nextElapsedMs = nextBeepElapsedMs(elapsedMs, alertAfterMs, alertIntervalMs);
  if (nextElapsedMs == 0 || nextElapsedMs <= elapsedMs) {
    return 0;
  }
  return nextElapsedMs - elapsedMs;
}

static unsigned long roundedSeconds(unsigned long ms) {
  return (ms + 500UL) / 1000UL;
}

static void appendUnsigned(String& out, unsigned long value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", value);
  out += buf;
}

static void appendSigned(String& out, long value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld", value);
  out += buf;
}

static void setStatus(const String& s) {
  if (statusMutex != nullptr) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
  }
  statusLine = s;
  if (statusMutex != nullptr) {
    xSemaphoreGive(statusMutex);
  }
  Serial.println(s);
}

static String getStatusLine() {
  if (statusMutex != nullptr) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
  }
  String current = statusLine;
  if (statusMutex != nullptr) {
    xSemaphoreGive(statusMutex);
  }
  return current;
}

static void logWiFiConnectAttempt(const char* phase, wl_status_t status) {
  String line;
  line.reserve(160);
  line += F("WIFI ");
  line += phase;
  line += F(" status=");
  line += wifiStatusName(status);
  line += F(" ssid='");
  line += cfg.ssid;
  line += '\'';
  Serial.println(line);
}

static String maskedPasswordTail3(const char* password) {
  if (password == nullptr) {
    return "";
  }

  const size_t len = strlen(password);
  if (len <= 3) {
    return String(password);
  }

  String masked;
  masked.reserve(len);
  for (size_t i = 0; i < len - 3; ++i) {
    masked += '*';
  }
  masked += (password + len - 3);
  return masked;
}

static void appendHtmlEscaped(String& out, const char* value) {
  if (value == nullptr) {
    return;
  }

  for (const char* p = value; *p != '\0'; ++p) {
    switch (*p) {
      case '&':
        out += F("&amp;");
        break;
      case '<':
        out += F("&lt;");
        break;
      case '>':
        out += F("&gt;");
        break;
      case '"':
        out += F("&quot;");
        break;
      case '\'':
        out += F("&#39;");
        break;
      default:
        out += *p;
        break;
    }
  }
}

static uint64_t parseUint64(const char* s) {
  char* end = nullptr;
  unsigned long long n = strtoull(s, &end, 10);
  if (end == s) {
    return 0;
  }
  return static_cast<uint64_t>(n);
}

static long parseLong(const char* s) {
  char* end = nullptr;
  long n = strtol(s, &end, 10);
  if (end == s) {
    return 0;
  }
  return n;
}

static void trimInPlace(char* s) {
  if (s == nullptr) {
    return;
  }

  char* start = s;
  while (*start != '\0' && isspace(static_cast<unsigned char>(*start))) {
    start++;
  }

  char* end = start + strlen(start);
  while (end > start && isspace(static_cast<unsigned char>(*(end - 1)))) {
    end--;
  }
  *end = '\0';

  if (start != s) {
    memmove(s, start, static_cast<size_t>(end - start) + 1U);
  }
}

static String apiBaseFromHost(const String& apiHost) {
  String host = apiHost;
  host.trim();
  if (host.startsWith("https://")) {
    host.remove(0, 8);
  } else if (host.startsWith("http://")) {
    host.remove(0, 7);
  }
  if (!host.length()) {
    return "";
  }
  if (host.startsWith("/")) {
    return "";
  }
  int slash = host.indexOf('/');
  if (slash >= 0) {
    host = host.substring(0, slash);
  }
  if (!host.length()) {
    return "";
  }
  if (host.indexOf(' ') >= 0) {
    return "";
  }
  while (host.endsWith("/")) {
    host.remove(host.length() - 1);
  }
  if (!host.length()) {
    return "";
  }

  if (!host.startsWith("http://")) {
    host = "http://" + host;
  }
  return host + "/api";
}

static void refreshWebhookBase() {
  String apiBase = apiBaseFromHost(String(cfg.apiHost));
  if (!apiBase.length()) {
    webhookBase = "";
    deviceStateUrl = "";
    return;
  }

  webhookBase = apiBase + "/timer";
  deviceStateUrl = apiBase + "/device-state?id=" + suffix;
}

void loadConfig() {
  prefs.begin("remote-timer", true);

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String host = prefs.getString("api_host", cfg.apiHost);
  String token = prefs.getString("api_token", "");

  prefs.end();

  if (!host.length()) {
    host = cfg.apiHost;
  }

  ssid.toCharArray(cfg.ssid, sizeof(cfg.ssid));
  pass.toCharArray(cfg.password, sizeof(cfg.password));
  host.toCharArray(cfg.apiHost, sizeof(cfg.apiHost));
  token.toCharArray(cfg.bearerToken, sizeof(cfg.bearerToken));
  refreshWebhookBase();
}

void saveConfig() {
  prefs.begin("remote-timer", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.password);
  prefs.putString("api_host", cfg.apiHost);
  prefs.putString("api_token", cfg.bearerToken);
  prefs.end();
}

void beginWiFiConnect() {
  if (!cfg.ssid[0]) {
    setStatus("No WiFi SSID configured");
    return;
  }

  const wl_status_t status = WiFi.status();
  logWiFiConnectAttempt("attempt", status);
  if (status == WL_CONNECTED) {
    setStatus("WiFi connected");
    return;
  }

  const wl_status_t beginStatus = WiFi.begin(cfg.ssid, cfg.password);
  if (beginStatus == WL_CONNECT_FAILED) {
    logWiFiConnectAttempt("request_failed", WiFi.status());
    setStatus("WiFi connect request failed");
    return;
  }

  deviceStateLoaded = false;
  lastDeviceStateFetchAttempt = 0;
  String connectMsg = "Connecting WiFi...";
  connectMsg += " ssid=";
  connectMsg += cfg.ssid;
  if (!wifiPasswordShownAtBoot) {
    connectMsg += " pass=";
    connectMsg += maskedPasswordTail3(cfg.password);
    wifiPasswordShownAtBoot = true;
  }
  setStatus(connectMsg);
}

void handleRoot() {
  String status = getStatusLine();
  String page;
  page.reserve(2000);
  page += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Remote Timer</title></head><body style='font-family:sans-serif;font-size:1.05rem;line-height:1.45;margin:0;padding:0.85rem;'><main>");
  page += F("<h3>Remote Timer</h3>");
  page += F("<p>Status: ");
  appendHtmlEscaped(page, status.c_str());
  page += F("</p>");
  if (WiFi.status() == WL_CONNECTED) {
    String wifiSsid = WiFi.SSID();
    page += F("<p>WiFi: ");
    appendHtmlEscaped(page, wifiSsid.c_str());
    page += F(" ");
    page += WiFi.localIP().toString();
    page += F("</p>");
  } else {
    page += F("<p>WiFi: not connected</p>");
  }

  page += F("<form method='POST' action='/save'>");
  page += F("<label>SSID<br><input style='font-size:1rem;width:100%;' name='ssid' maxlength='32' value='");
  appendHtmlEscaped(page, cfg.ssid);
  page += F("'></label>");
  page += F("<br><label>Password<br><input style='font-size:1rem;width:100%;' type='password' name='password' maxlength='64' value='");
  appendHtmlEscaped(page, cfg.password);
  page += F("'></label>");
  page += F("<br><label>API Host (host:port)<br><input style='font-size:1rem;width:100%;' name='api_host' maxlength='128' value='");
  appendHtmlEscaped(page, cfg.apiHost);
  page += F("'></label>");
  page += F("<br><label>API Token (optional)<br><input style='font-size:1rem;width:100%;' type='password' name='api_token' maxlength='128' value='");
  appendHtmlEscaped(page, cfg.bearerToken);
  page += F("'></label>");
  page += F("<br><button style='font-size:1rem;' type='submit'>Save</button></form>");
  page += F("<p><a href='/debug'>debug</a></p>");
  page += F("</main></body></html>");

  configServer.send(200, "text/html", page);
}

void copyFormField(const String& value, char* target, size_t maxSize) {
  String v = value;
  v.trim();
  v.toCharArray(target, maxSize);
}

void handleSave() {
  copyFormField(configServer.arg("ssid"), cfg.ssid, sizeof(cfg.ssid));
  copyFormField(configServer.arg("password"), cfg.password, sizeof(cfg.password));
  copyFormField(configServer.arg("api_host"), cfg.apiHost, sizeof(cfg.apiHost));
  copyFormField(configServer.arg("api_token"), cfg.bearerToken, sizeof(cfg.bearerToken));

  refreshWebhookBase();
  if (!cfg.apiHost[0] || !webhookBase.length()) {
    configServer.send(400, "text/html", "<h2>Invalid API config</h2><a href='/'>Back</a>");
    return;
  }

  saveConfig();
  configServer.send(200, "text/html", F("<h2>Saved</h2><p>WiFi reconnect started. Check status on the main page.</p><a href='/'>Back</a>"));
  WiFi.disconnect(false, false);
  beginWiFiConnect();
}

void handleDebug() {
  const unsigned long now = millis();
  const wl_status_t wifiStatus = WiFi.status();
  const unsigned long freeHeap = ESP.getFreeHeap();
  const unsigned long minFreeHeap = ESP.getMinFreeHeap();
  const unsigned long maxAllocHeap = ESP.getMaxAllocHeap();
  const unsigned long fragPct = freeHeap == 0
    ? 0UL
    : 100UL - ((maxAllocHeap * 100UL) / freeHeap);
  String out;
  out.reserve(768);
  out += F("nowMs=");
  appendUnsigned(out, now);
  out += F(" loaded=");
  out += deviceStateLoaded ? '1' : '0';
  out += F(" lastPollMs=");
  appendUnsigned(out, lastDeviceStateFetchAttempt);
  out += F(" freeHeap=");
  appendUnsigned(out, freeHeap);
  out += F(" minFreeHeap=");
  appendUnsigned(out, minFreeHeap);
  out += F(" maxAllocHeap=");
  appendUnsigned(out, maxAllocHeap);
  out += F(" fragPct=");
  appendUnsigned(out, fragPct);
  out += F(" wifiStatus=");
  out += wifiStatusName(wifiStatus);
  out += F(" wifiRssi=");
  appendSigned(out, lastWiFiRssi);
  out += F(" wifiCh=");
  appendSigned(out, lastWiFiChannel);
  out += F(" wifiDiscReason=");
  appendSigned(out, lastWiFiDisconnectReason);
  out += '\n';

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    const unsigned long elapsedMs = effectiveElapsedMs(i, now);
    const unsigned long nextBeepMs = (!timerActiveByButton[i] || alertMutedByButton[i])
      ? 0UL
      : nextBeepInMs(elapsedMs, alertAfterMsByButton[i], alertIntervalMsByButton[i]);
    out += 'b';
    out += static_cast<char>('1' + i);
    out += F(" muted=");
    out += alertMutedByButton[i] ? '1' : '0';
    out += F(" elapsed=");
    appendUnsigned(out, roundedSeconds(elapsedMs));
    out += F(" after=");
    appendUnsigned(out, roundedSeconds(alertAfterMsByButton[i]));
    out += F(" interval=");
    appendUnsigned(out, roundedSeconds(alertIntervalMsByButton[i]));
    out += F(" nextBeep=");
    appendUnsigned(out, roundedSeconds(nextBeepMs));
    out += '\n';
  }

  configServer.send(200, "text/plain", out);
}

void handleReboot() {
  rebootRequested = true;
  rebootAt = millis() + 300;
  setStatus("Reboot requested");
  configServer.send(200, "text/plain", "rebooting\n");
}

void ensureConfigServerStarted() {
  if (!configPortalStarted) {
    apSsid = String("RemoteTimer-") + suffix;
    configServer.on("/", HTTP_GET, handleRoot);
    configServer.on("/debug", HTTP_GET, handleDebug);
    configServer.on("/save", HTTP_POST, handleSave);
    configServer.on("/reboot", HTTP_POST, handleReboot);
    configServer.on("/reboot", HTTP_GET, handleReboot);
    configServer.onNotFound(handleRoot);
    configServer.begin();
    configPortalStarted = true;
  }
}

void startPortal() {
  ensureConfigServerStarted();
  if (configApEnabled) {
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str());
  configApEnabled = true;
  setStatus("Config AP: " + apSsid);
}

void updateConfigApAutoDisable(unsigned long now) {
  if (!configApEnabled || !apDisablePending) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if ((long)(now - apDisableAt) < 0) {
    return;
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  configApEnabled = false;
  apDisablePending = false;
  setStatus("Config AP disabled");
  Serial.println("WIFI AP disabled after stable STA connection");
}

bool performTimerAction(uint8_t buttonIndex, const char* action) {
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("API skipped: no WiFi");
    return false;
  }

  if (!webhookBase.length()) {
    setStatus("API skipped: invalid host");
    return false;
  }

  char idValue[20];
  snprintf(idValue, sizeof(idValue), "%s-%u", suffix.c_str(), static_cast<unsigned>(buttonIndex + 1));

  char url[220];
  const char* base = webhookBase.c_str();
  const char* separator = strchr(base, '?') ? "&id=" : "?id=";
  snprintf(url, sizeof(url), "%s%s%s", base, separator, idValue);

  if (networkMutex != nullptr) {
    if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(NetCfg::LockTimeoutMs)) != pdTRUE) {
      setStatus("API busy");
      return false;
    }
  }

  HTTPClient http;
  WiFiClient client;
  http.setTimeout(NetCfg::HttpTimeoutMs);
  http.begin(client, url);
  if (cfg.bearerToken[0]) {
    http.addHeader("Authorization", String("Bearer ") + cfg.bearerToken);
  }

  int code = 0;
  if (strcmp(action, "delete") == 0) {
    code = http.sendRequest("DELETE");
  } else {
    code = http.sendRequest("POST");
  }

  if (code >= 200 && code < 300) {
    http.end();
    if (networkMutex != nullptr) {
      xSemaphoreGive(networkMutex);
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%s btn %u OK (%d)", action, static_cast<unsigned>(buttonIndex + 1), code);
    setStatus(msg);
    return true;
  }

  http.end();
  if (networkMutex != nullptr) {
    xSemaphoreGive(networkMutex);
  }
  char fail[80];
  snprintf(
    fail,
    sizeof(fail),
    "%s btn %u fail (%d)",
    action,
    static_cast<unsigned>(buttonIndex + 1),
    code
  );
  setStatus(fail);
  return false;
}

bool enqueueTimerAction(uint8_t buttonIndex, bool isDelete) {
  if (actionQueue == nullptr) {
    setStatus("API queue unavailable");
    return false;
  }

  TimerAction action;
  action.buttonIndex = buttonIndex;
  action.isDelete = isDelete;

  BaseType_t queued = xQueueSend(actionQueue, &action, 0);
  if (queued != pdTRUE) {
    setStatus("API queue full");
    return false;
  }

  char msg[40];
  snprintf(msg, sizeof(msg), "%s btn %u queued", isDelete ? "delete" : "set", static_cast<unsigned>(buttonIndex + 1));
  setStatus(msg);
  return true;
}

bool enqueueTonePattern(uint8_t buttonIndex, bool isLongPress, uint8_t beepCount) {
  if (buttonIndex >= BUTTON_COUNT || toneQueue == nullptr) {
    return false;
  }

  TonePattern pattern;
  pattern.frequency = isLongPress ? LONG_PRESS_TONES[buttonIndex] : SHORT_PRESS_TONES[buttonIndex];
  pattern.beepsRemaining = beepCount;
  pattern.toneMs = TimingCfg::ToneMs;
  pattern.gapMs = TimingCfg::ToneGapMs;

  const bool ok = xQueueSend(toneQueue, &pattern, 0) == pdTRUE;
  if (ok) {
    actionToneEnqueueCountByButton[buttonIndex]++;
    Serial.print("ENQ tone action b");
    Serial.print(static_cast<unsigned>(buttonIndex + 1));
    Serial.print(" type=");
    Serial.print(isLongPress ? "long" : "short");
    Serial.print(" beeps=");
    Serial.print(static_cast<unsigned>(beepCount));
    Serial.print(" freq=");
    Serial.println(static_cast<unsigned>(pattern.frequency));
  } else {
    Serial.print("ENQ tone action FAILED b");
    Serial.println(static_cast<unsigned>(buttonIndex + 1));
  }
  return ok;
}

bool enqueueAlertTonePattern(uint8_t buttonIndex) {
  if (buttonIndex >= BUTTON_COUNT || toneQueue == nullptr) {
    return false;
  }

  TonePattern pattern;
  pattern.frequency = SHORT_PRESS_TONES[buttonIndex];
  pattern.beepsRemaining = TimingCfg::AlertToneBeeps;
  pattern.toneMs = TimingCfg::AlertToneMs;
  pattern.gapMs = TimingCfg::AlertToneGapMs;

  const bool ok = xQueueSend(toneQueue, &pattern, 0) == pdTRUE;
  if (ok) {
    alertToneEnqueueCountByButton[buttonIndex]++;
    Serial.print("ENQ tone alert b");
    Serial.print(static_cast<unsigned>(buttonIndex + 1));
    Serial.print(" beeps=");
    Serial.print(static_cast<unsigned>(pattern.beepsRemaining));
    Serial.print(" freq=");
    Serial.print(static_cast<unsigned>(pattern.frequency));
    Serial.print(" elapsed=");
    Serial.print(effectiveElapsedMs(buttonIndex, millis()));
    Serial.print(" after=");
    Serial.print(alertAfterMsByButton[buttonIndex]);
    Serial.print(" interval=");
    Serial.println(alertIntervalMsByButton[buttonIndex]);
  } else {
    Serial.print("ENQ tone alert FAILED b");
    Serial.println(static_cast<unsigned>(buttonIndex + 1));
  }
  return ok;
}

void enqueueStartupToneSeries() {
  if (toneQueue == nullptr) {
    return;
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    TonePattern pattern;
    pattern.frequency = SHORT_PRESS_TONES[i];
    pattern.beepsRemaining = 1;
    pattern.toneMs = TimingCfg::ToneMs;
    pattern.gapMs = TimingCfg::ToneGapMs;
    xQueueSend(toneQueue, &pattern, 0);
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    TonePattern pattern;
    pattern.frequency = LONG_PRESS_TONES[i];
    pattern.beepsRemaining = 1;
    pattern.toneMs = TimingCfg::ToneMs;
    pattern.gapMs = TimingCfg::ToneGapMs;
    xQueueSend(toneQueue, &pattern, 0);
  }
}

void enqueueStartupToneSeriesReversed() {
  if (toneQueue == nullptr) {
    return;
  }

  // Reverse of startup: long tones 4->1, then short tones 4->1.
  for (int i = static_cast<int>(BUTTON_COUNT) - 1; i >= 0; i--) {
    TonePattern pattern;
    pattern.frequency = LONG_PRESS_TONES[static_cast<uint8_t>(i)];
    pattern.beepsRemaining = 1;
    pattern.toneMs = TimingCfg::ToneMs;
    pattern.gapMs = TimingCfg::ToneGapMs;
    xQueueSend(toneQueue, &pattern, 0);
  }

  for (int i = static_cast<int>(BUTTON_COUNT) - 1; i >= 0; i--) {
    TonePattern pattern;
    pattern.frequency = SHORT_PRESS_TONES[static_cast<uint8_t>(i)];
    pattern.beepsRemaining = 1;
    pattern.toneMs = TimingCfg::ToneMs;
    pattern.gapMs = TimingCfg::ToneGapMs;
    xQueueSend(toneQueue, &pattern, 0);
  }
}

void runSelfTestMelody() {
  Serial.println("Self-test requested: startup tones in reverse");
  enqueueStartupToneSeriesReversed();
  setStatus("Self-test queued");
}

void queueLoadedTimerTones() {
  uint8_t pendingMask = 0;
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (timerActiveByButton[i]) {
      pendingMask |= static_cast<uint8_t>(1U << i);
    }
  }
  loadedTimerTonePendingMask = pendingMask;
  loadedTimerToneStarted = false;
  loadedTimerToneNextAt = 0;
}

void flushLoadedTimerToneQueue(unsigned long now) {
  if (loadedTimerTonePendingMask == 0) {
    return;
  }
  if (toneQueue == nullptr) {
    return;
  }

  // Start identify tones only after startup sequence fully drains.
  const bool queueIdle = uxQueueMessagesWaiting(toneQueue) == 0;
  if (!loadedTimerToneStarted) {
    if (!queueIdle || tonePatternActive) {
      return;
    }
    loadedTimerToneStarted = true;
    loadedTimerToneNextAt = now + TimingCfg::LoadedTimerToneStartDelayMs;
    return;
  }

  if (!queueIdle || tonePatternActive || (long)(now - loadedTimerToneNextAt) < 0) {
    return;
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    const uint8_t bit = static_cast<uint8_t>(1U << i);
    if ((loadedTimerTonePendingMask & bit) == 0) {
      continue;
    }
    if (enqueueTonePattern(i, false, 1)) {
      loadedTimerTonePendingMask = static_cast<uint8_t>(loadedTimerTonePendingMask & ~bit);
      loadedTimerToneNextAt = now + TimingCfg::LoadedTimerToneSpacingMs;
      break;
    }
  }
}

void applyTimerActionToAlertState(uint8_t buttonIndex, bool isDelete) {
  if (buttonIndex >= BUTTON_COUNT) {
    return;
  }

  if (isDelete) {
    timerActiveByButton[buttonIndex] = false;
    timerElapsedBaseMsByButton[buttonIndex] = 0;
    timerElapsedBaseAtMsByButton[buttonIndex] = 0;
    timerEpochMsByButton[buttonIndex] = 0;
    lastAlertIndex[buttonIndex] = -1;
    return;
  }

  timerActiveByButton[buttonIndex] = true;
  timerElapsedBaseMsByButton[buttonIndex] = 0;
  timerElapsedBaseAtMsByButton[buttonIndex] = millis();
  timerEpochMsByButton[buttonIndex] = 0;
  lastAlertIndex[buttonIndex] = -1;
}

void processTimerActions() {
  if (actionQueue == nullptr) {
    return;
  }

  TimerAction action;
  // Drain a small bounded batch each loop to keep UI/button handling responsive.
  uint8_t processed = 0;
  while (processed < LimitsCfg::ActionQueueLen && xQueueReceive(actionQueue, &action, 0) == pdTRUE) {
    if (performTimerAction(action.buttonIndex, action.isDelete ? "delete" : "set")) {
      applyTimerActionToAlertState(action.buttonIndex, action.isDelete);
      enqueueTonePattern(action.buttonIndex, action.isDelete, 1);
    }
    processed++;
  }
}

void startTonePattern(const TonePattern& pattern, unsigned long now) {
  if (pattern.frequency == 0 || pattern.beepsRemaining == 0) {
    return;
  }

  currentTonePattern = pattern;
  tonePatternActive = true;
  toneOutputActive = true;
  ledcWriteTone(BUZZER_PIN, currentTonePattern.frequency);
  toneStageEndsAt = now + currentTonePattern.toneMs;
}

void updateTone(unsigned long now) {
  if (!tonePatternActive && toneQueue != nullptr) {
    TonePattern nextPattern;
    if (xQueueReceive(toneQueue, &nextPattern, 0) == pdTRUE) {
      startTonePattern(nextPattern, now);
    }
  }

  if (!tonePatternActive || (long)(now - toneStageEndsAt) < 0) {
    return;
  }

  if (toneOutputActive) {
    ledcWriteTone(BUZZER_PIN, 0);
    toneOutputActive = false;
    currentTonePattern.beepsRemaining--;

    if (currentTonePattern.beepsRemaining == 0) {
      tonePatternActive = false;
    } else {
      toneStageEndsAt = now + currentTonePattern.gapMs;
    }
    return;
  }

  toneOutputActive = true;
  ledcWriteTone(BUZZER_PIN, currentTonePattern.frequency);
  toneStageEndsAt = now + currentTonePattern.toneMs;
}

void handleButtonAction(uint8_t i, bool isLongPress) {
  if (isLongPress) {
    Serial.print("Button ");
    Serial.print(i);
    Serial.println(" - Action: LONG PRESS (Delete)");
    enqueueTonePattern(i, true, 1);
    enqueueTimerAction(i, true);
  } else {
    Serial.print("Button Index: ");
    Serial.print(i);
    Serial.println(" - Action: SHORT PRESS (Set)");
    enqueueTonePattern(i, false, 1);
    enqueueTimerAction(i, false);
  }
}

void updateBoardButton(unsigned long now) {
  // On-board button is active LOW with pull-up.
  const bool rawPressed = digitalRead(RIGHT_BOARD_BUTTON_PIN) == LOW;

  if (rawPressed != boardBtnLastRawPressed) {
    boardBtnLastRawPressed = rawPressed;
    boardBtnRawChangedAt = now;
  }

  if ((now - boardBtnRawChangedAt) < TimingCfg::BoardButtonDebounceMs) {
    return;
  }

  if ((long)(now - boardBtnCooldownUntil) < 0) {
    boardBtnStablePressed = rawPressed;
    return;
  }

  if (rawPressed != boardBtnStablePressed) {
    boardBtnStablePressed = rawPressed;
    // Trigger on release after a valid press.
    if (!boardBtnStablePressed) {
      runSelfTestMelody();
      boardBtnCooldownUntil = now + TimingCfg::BoardButtonCooldownMs;
    }
  }
}

void updateButtons() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    ButtonState& b = buttons[i];

    // Assumes YK04 receiver output is active HIGH while button held.
    bool rawPressed = digitalRead(BUTTON_PINS[i]) == HIGH;

    if (rawPressed != b.lastRawPressed) {
      b.lastRawPressed = rawPressed;
      b.rawChangedAt = now;
    }

    if ((now - b.rawChangedAt) < TimingCfg::ButtonDebounceMs) {
      continue;
    }

    if ((long)(now - b.cooldownUntil) < 0) {
      // Swallow transitions during post-trigger cooldown.
      b.stablePressed = rawPressed;
      b.longTriggered = rawPressed;
      continue;
    }

    if (rawPressed != b.stablePressed) {
      b.stablePressed = rawPressed;

      if (b.stablePressed) {
        b.pressedAt = now;
        b.longTriggered = false;
      } else {
        if (!b.longTriggered) {
          handleButtonAction(i, false);
          b.cooldownUntil = now + TimingCfg::ButtonPressCooldownMs;
        }
      }
    }

    if (b.stablePressed && !b.longTriggered && (now - b.pressedAt >= TimingCfg::ButtonLongPressMs)) {
      b.longTriggered = true;
      handleButtonAction(i, true);
      b.cooldownUntil = now + TimingCfg::ButtonPressCooldownMs;
    }
  }
}

void pollDeviceState(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED || !deviceStateUrl.length()) {
    return;
  }
  if (lastDeviceStateFetchAttempt != 0 && (now - lastDeviceStateFetchAttempt < TimingCfg::DeviceStatePollMs)) {
    return;
  }
  lastDeviceStateFetchAttempt = now;

  if (networkMutex != nullptr) {
    if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(NetCfg::LockTimeoutMs)) != pdTRUE) {
      return;
    }
  }

  HTTPClient http;
  WiFiClient client;
  http.setTimeout(NetCfg::HttpTimeoutMs);
  http.begin(client, deviceStateUrl);
  if (cfg.bearerToken[0]) {
    http.addHeader("Authorization", String("Bearer ") + cfg.bearerToken);
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    if (networkMutex != nullptr) {
      xSemaphoreGive(networkMutex);
    }
    return;
  }

  const bool suppressInitialDueBeeps = !deviceStateLoaded;
  uint64_t nowEpochMs = 0;
  bool seenButton[BUTTON_COUNT] = {false, false, false, false};
  WiFiClient* stream = http.getStreamPtr();
  char line[160];
  while (stream != nullptr && (stream->connected() || stream->available())) {
    const size_t len = stream->readBytesUntil('\n', line, sizeof(line) - 1U);
    if (len == 0) {
      if (!stream->available()) {
        break;
      }
      continue;
    }

    line[len] = '\0';
    trimInPlace(line);
    if (line[0] == '\0') {
      continue;
    }

    char* c1 = strchr(line, ',');
    if (c1 == nullptr || c1 == line) {
      continue;
    }
    *c1 = '\0';

    if (strcmp(line, "now") == 0) {
      nowEpochMs = parseUint64(c1 + 1);
      continue;
    }

    char* c2 = strchr(c1 + 1, ',');
    char* c3 = c2 != nullptr ? strchr(c2 + 1, ',') : nullptr;
    char* c4 = c3 != nullptr ? strchr(c3 + 1, ',') : nullptr;
    if (c2 == nullptr || c3 == nullptr || c4 == nullptr) {
      continue;
    }

    *c2 = '\0';
    *c3 = '\0';
    *c4 = '\0';

    const long button = parseLong(line);
    if (button < 1 || button > static_cast<long>(BUTTON_COUNT)) {
      continue;
    }

    const uint8_t idx = static_cast<uint8_t>(button - 1);
    const uint64_t timestampMs = parseUint64(c1 + 1);
    const int mutedInt = static_cast<int>(parseLong(c2 + 1));
    const unsigned long alertAfterMs = static_cast<unsigned long>(parseUint64(c3 + 1));
    const unsigned long alertIntervalMs = static_cast<unsigned long>(parseUint64(c4 + 1));

    alertMutedByButton[idx] = mutedInt != 0;
    alertAfterMsByButton[idx] = alertAfterMs;
    alertIntervalMsByButton[idx] = alertIntervalMs;

    if (alertMutedByButton[idx]) {
      // Fully drop muted timers from local processing.
      timerActiveByButton[idx] = false;
      timerElapsedBaseMsByButton[idx] = 0;
      timerElapsedBaseAtMsByButton[idx] = 0;
      timerEpochMsByButton[idx] = 0;
      lastAlertIndex[idx] = -1;
      seenButton[idx] = true;
      continue;
    }

    if (timestampMs > 0 && nowEpochMs > 0 && nowEpochMs >= timestampMs) {
      const bool timerChanged = timerEpochMsByButton[idx] != timestampMs;
      timerEpochMsByButton[idx] = timestampMs;
      const uint64_t elapsedMs64 = nowEpochMs - timestampMs;
      const unsigned long nowMs = millis();
      timerActiveByButton[idx] = true;
      timerElapsedBaseMsByButton[idx] = elapsedMs64;
      timerElapsedBaseAtMsByButton[idx] = nowMs;

      if (suppressInitialDueBeeps) {
        const unsigned long elapsedMs =
          elapsedMs64 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<unsigned long>(elapsedMs64);
        // On first load, seed alert index from current elapsed to avoid immediate overdue beeps.
        lastAlertIndex[idx] = alertIndexForElapsed(elapsedMs, alertAfterMs, alertIntervalMs);
      } else if (timerChanged) {
        // On a newly observed timer timestamp, reset alert index so the next
        // local tick can trigger the first due alert beep at/after alertAfter.
        lastAlertIndex[idx] = -1;
      }
    } else {
      timerActiveByButton[idx] = false;
      timerElapsedBaseMsByButton[idx] = 0;
      timerElapsedBaseAtMsByButton[idx] = 0;
      timerEpochMsByButton[idx] = 0;
      lastAlertIndex[idx] = -1;
    }

    seenButton[idx] = true;
  }
  http.end();
  if (networkMutex != nullptr) {
    xSemaphoreGive(networkMutex);
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (!seenButton[i]) {
      alertMutedByButton[i] = false;
      alertAfterMsByButton[i] = 0;
      alertIntervalMsByButton[i] = 0;
      timerActiveByButton[i] = false;
      timerElapsedBaseMsByButton[i] = 0;
      timerElapsedBaseAtMsByButton[i] = 0;
      timerEpochMsByButton[i] = 0;
      lastAlertIndex[i] = -1;
    }
  }

  if (!deviceStateLoaded) {
    deviceStateLoaded = true;
    if (!loadedTimerTonesPlayedThisBoot) {
      loadedTimerTonesPlayedThisBoot = true;
      queueLoadedTimerTones();
    }
    setStatus("Loaded device alert state");
  }
}

void updateLocalAlerts(unsigned long now) {
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (!timerActiveByButton[i] || alertMutedByButton[i] || alertAfterMsByButton[i] == 0) {
      lastAlertIndex[i] = -1;
      continue;
    }

    const unsigned long elapsedMs = effectiveElapsedMs(i, now);
    if (elapsedMs < alertAfterMsByButton[i]) {
      lastAlertIndex[i] = -1;
      continue;
    }

    int32_t alertIndex = 0;
    if (alertIntervalMsByButton[i] > 0) {
      alertIndex = static_cast<int32_t>((elapsedMs - alertAfterMsByButton[i]) / alertIntervalMsByButton[i]);
    }

    if (alertIndex != lastAlertIndex[i]) {
      lastAlertIndex[i] = alertIndex;
      enqueueAlertTonePattern(i);
    }
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);

  Serial.begin(115200);
  Serial.println("=== ESP32-C3 Remote Timer Boot ===");
  statusMutex = xSemaphoreCreateMutex();
  networkMutex = xSemaphoreCreateMutex();
  actionQueue = xQueueCreate(LimitsCfg::ActionQueueLen, sizeof(TimerAction));
  toneQueue = xQueueCreate(LimitsCfg::ToneQueueLen, sizeof(TonePattern));
  uint64_t mac = ESP.getEfuseMac();
  char suffixBuf[7];
  snprintf(suffixBuf, sizeof(suffixBuf), "%06llX", mac & 0xFFFFFFULL);
  suffix = String(suffixBuf);

  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);
  pinMode(RIGHT_BOARD_BUTTON_PIN, INPUT_PULLUP);

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT);
  }

  enqueueStartupToneSeries();

  loadConfig();
  WiFi.mode(WIFI_STA);
  ensureConfigServerStarted();
  WiFi.onEvent(onWiFiEvent);
  WiFi.setAutoReconnect(true);
  if (!cfg.ssid[0]) {
    startupWaitForWiFi = false;
    startPortal();
  }
  if (actionQueue == nullptr) {
    setStatus("API queue init failed");
  }

  beginWiFiConnect();
}

void loop() {
  unsigned long now = millis();

  configServer.handleClient();
  if (wifiRecoveryPending && (long)(now - wifiRecoveryAt) >= 0) {
    if (wifiRecoveryStage == 1) {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(true, true);
      wifiRecoveryStage = 2;
      wifiRecoveryAt = now + WiFiCfg::RecoveryBackoffMs;
      setStatus("WiFi recovery backoff before reconnect");
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      beginWiFiConnect();
      wifiRecoveryPending = false;
      wifiRecoveryStage = 0;
      wifiAuthExpireStreak = 0;
    }
  }
  if (startupWaitForWiFi && WiFi.status() != WL_CONNECTED) {
    updateWiFiDiagnostics(now);
    updateConfigApAutoDisable(now);
    delay(TimingCfg::LoopDelayMs);
    return;
  }
  updateButtons();
  updateBoardButton(now);
  processTimerActions();
  pollDeviceState(now);
  now = millis();
  updateLocalAlerts(now);
  flushLoadedTimerToneQueue(now);
  updateTone(now);
  updateWiFiDiagnostics(now);
  updateConfigApAutoDisable(now);

  if (now - lastLedToggle >= TimingCfg::LedToggleMs) {
    lastLedToggle = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  if (rebootRequested && (long)(now - rebootAt) >= 0) {
    ESP.restart();
  }

  delay(TimingCfg::LoopDelayMs);
}
