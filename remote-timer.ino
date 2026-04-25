#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <stdlib.h>

#define LED_PIN 8
unsigned long lastLedToggle = 0;
bool ledState = false;

// Buzzer output pin (adjust for your ESP32-C3 wiring).
constexpr uint8_t BUZZER_PIN = 10;
constexpr uint16_t SHORT_PRESS_TONES[4] = {523, 587, 659, 698}; // Do Re Mi Fa (C5 D5 E5 F5)
constexpr uint16_t LONG_PRESS_TONES[4] = {784, 880, 988, 1047}; // So La Ti Do (G5 A5 B5 C6)
constexpr unsigned long TONE_MS = 90;
constexpr unsigned long TONE_GAP_MS = 70;

// YK04 receiver outputs (adjust pins to match your board wiring).
constexpr uint8_t BUTTON_PINS[4] = {0, 1, 3, 4};
constexpr uint8_t BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
constexpr unsigned long DEBOUNCE_MS = 100;
constexpr unsigned long LONG_PRESS_MS = 3000;
constexpr unsigned long PRESS_COOLDOWN_MS = 1000;
constexpr unsigned long DEVICE_STATE_POLL_MS = 60000UL;
constexpr unsigned long ALERT_TONE_MS = 400UL;
constexpr unsigned long ALERT_TONE_GAP_MS = 100UL;
constexpr uint8_t ALERT_TONE_BEEPS = 2;
constexpr unsigned long LOADED_TIMER_TONE_START_DELAY_MS = 400UL;
constexpr unsigned long LOADED_TIMER_TONE_SPACING_MS = 280UL;

// Wi-Fi and config portal.
constexpr unsigned long WIFI_CONNECT_TIMEOUT = 15000UL;
constexpr unsigned long WIFI_RETRY_INTERVAL = 20000UL;

constexpr size_t SSID_MAX_LEN = 32;
constexpr size_t PASS_MAX_LEN = 64;
constexpr size_t HOST_MAX_LEN = 128;
constexpr size_t TOKEN_MAX_LEN = 128;
constexpr uint8_t ACTION_QUEUE_LEN = 4;
constexpr uint8_t TONE_QUEUE_LEN = 16;
constexpr unsigned long LED_TOGGLE_MS = 1000UL;
constexpr unsigned long LOOP_DELAY_MS = 5UL;

struct DeviceConfig {
  char ssid[SSID_MAX_LEN + 1] = {};
  char password[PASS_MAX_LEN + 1] = {};
  char apiHost[HOST_MAX_LEN + 1] = "192.168.1.101:30109";
  char bearerToken[TOKEN_MAX_LEN + 1] = {};
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

unsigned long lastReconnectAttempt = 0;
bool wifiConnectInProgress = false;
unsigned long wifiConnectStartedAt = 0;

String statusLine = "Booting";
unsigned long statusUpdatedAt = 0;

String apSsid;
String deviceId;
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
bool wifiReconnectRequested = false;
unsigned long wifiReconnectAt = 0;

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

static void setStatus(const String& s) {
  if (statusMutex != nullptr) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
  }
  statusLine = s;
  statusUpdatedAt = millis();
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

static String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#39;";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

static uint64_t parseUint64(const String& value) {
  const char* s = value.c_str();
  char* end = nullptr;
  unsigned long long n = strtoull(s, &end, 10);
  if (end == s) {
    return 0;
  }
  return static_cast<uint64_t>(n);
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
    wifiConnectInProgress = false;
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.ssid, cfg.password);
  wifiConnectInProgress = true;
  deviceStateLoaded = false;
  lastDeviceStateFetchAttempt = 0;
  wifiConnectStartedAt = millis();
  lastReconnectAttempt = wifiConnectStartedAt;
  setStatus("Connecting WiFi...");
}

void updateWiFiConnection(unsigned long now) {
  if (!wifiConnectInProgress) {
    return;
  }

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnectInProgress = false;
    char msg[48];
    snprintf(msg, sizeof(msg), "WiFi connected: %s", WiFi.localIP().toString().c_str());
    setStatus(msg);
    return;
  }

  if (now - wifiConnectStartedAt >= WIFI_CONNECT_TIMEOUT) {
    wifiConnectInProgress = false;
    WiFi.disconnect();
    setStatus("WiFi connect failed");
  }
}

void handleRoot() {
  String escapedStatus = htmlEscape(getStatusLine());
  String escapedSsid = htmlEscape(String(cfg.ssid));
  String escapedPassword = htmlEscape(String(cfg.password));
  String escapedApiHost = htmlEscape(String(cfg.apiHost));
  String escapedBearerToken = htmlEscape(String(cfg.bearerToken));
  String page;
  page.reserve(2000);
  page += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Remote Timer</title></head><body style='font-family:sans-serif;font-size:1.05rem;line-height:1.45;margin:0;padding:0.85rem;'><main>");
  page += F("<h3>Remote Timer</h3>");
  page += F("<p>Status: ");
  page += escapedStatus;
  page += F("</p>");
  if (WiFi.status() == WL_CONNECTED) {
    page += F("<p>WiFi: ");
    page += htmlEscape(WiFi.SSID());
    page += F(" ");
    page += WiFi.localIP().toString();
    page += F("</p>");
  } else {
    page += F("<p>WiFi: not connected</p>");
  }

  page += F("<form method='POST' action='/save'>");
  page += F("<label>SSID<br><input style='font-size:1rem;width:100%;' name='ssid' maxlength='32' value='");
  page += escapedSsid;
  page += F("'></label>");
  page += F("<br><label>Password<br><input style='font-size:1rem;width:100%;' type='password' name='password' maxlength='64' value='");
  page += escapedPassword;
  page += F("'></label>");
  page += F("<br><label>API Host (host:port)<br><input style='font-size:1rem;width:100%;' name='api_host' maxlength='128' value='");
  page += escapedApiHost;
  page += F("'></label>");
  page += F("<br><label>API Token (optional)<br><input style='font-size:1rem;width:100%;' type='password' name='api_token' maxlength='128' value='");
  page += escapedBearerToken;
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
  wifiReconnectRequested = true;
  wifiReconnectAt = millis() + 800UL;
}

void handleDebug() {
  const unsigned long now = millis();
  String out;
  out.reserve(448);
  out += "nowMs=" + String(now)
      + " loaded=" + String(deviceStateLoaded ? 1 : 0)
      + " lastPollMs=" + String(lastDeviceStateFetchAttempt)
      + "\n";

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    const unsigned long elapsedMs = effectiveElapsedMs(i, now);
    out += "b" + String(i + 1)
        + " muted=" + String(alertMutedByButton[i] ? 1 : 0)
        + " elapsed=" + String(elapsedMs)
        + " after=" + String(alertAfterMsByButton[i])
        + " interval=" + String(alertIntervalMsByButton[i])
        + "\n";
  }

  configServer.send(200, "text/plain", out);
}

void handleReboot() {
  rebootRequested = true;
  rebootAt = millis() + 300;
  setStatus("Reboot requested");
  configServer.send(200, "text/plain", "rebooting\n");
}

void startPortal() {
  
  apSsid = String("RemoteTimer-") + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str());

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/debug", HTTP_GET, handleDebug);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.on("/reboot", HTTP_POST, handleReboot);
  configServer.on("/reboot", HTTP_GET, handleReboot);
  configServer.onNotFound(handleRoot);
  configServer.begin();

  setStatus("Config AP: " + apSsid);
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

  HTTPClient http;
  WiFiClient client;
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
    char msg[64];
    snprintf(msg, sizeof(msg), "%s btn %u OK (%d)", action, static_cast<unsigned>(buttonIndex + 1), code);
    setStatus(msg);
    return true;
  }

  String resp = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  String fail = String(action) + " btn " + String(buttonIndex + 1) + " fail (" + String(code) + ") " + resp;
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
  pattern.toneMs = TONE_MS;
  pattern.gapMs = TONE_GAP_MS;

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
  pattern.beepsRemaining = ALERT_TONE_BEEPS;
  pattern.toneMs = ALERT_TONE_MS;
  pattern.gapMs = ALERT_TONE_GAP_MS;

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
    pattern.toneMs = TONE_MS;
    pattern.gapMs = TONE_GAP_MS;
    xQueueSend(toneQueue, &pattern, 0);
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    TonePattern pattern;
    pattern.frequency = LONG_PRESS_TONES[i];
    pattern.beepsRemaining = 1;
    pattern.toneMs = TONE_MS;
    pattern.gapMs = TONE_GAP_MS;
    xQueueSend(toneQueue, &pattern, 0);
  }
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
    loadedTimerToneNextAt = now + LOADED_TIMER_TONE_START_DELAY_MS;
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
      loadedTimerToneNextAt = now + LOADED_TIMER_TONE_SPACING_MS;
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

void timerActionTask(void* parameter) {
  TimerAction action;

  for (;;) {
    if (xQueueReceive(actionQueue, &action, portMAX_DELAY) == pdTRUE) {
      if (performTimerAction(action.buttonIndex, action.isDelete ? "delete" : "set")) {
        applyTimerActionToAlertState(action.buttonIndex, action.isDelete);
        enqueueTonePattern(action.buttonIndex, action.isDelete, 1);
      }
    }
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

    if ((now - b.rawChangedAt) < DEBOUNCE_MS) {
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
          b.cooldownUntil = now + PRESS_COOLDOWN_MS;
        }
      }
    }

    if (b.stablePressed && !b.longTriggered && (now - b.pressedAt >= LONG_PRESS_MS)) {
      b.longTriggered = true;
      handleButtonAction(i, true);
      b.cooldownUntil = now + PRESS_COOLDOWN_MS;
    }
  }
}

void pollDeviceState(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED || !deviceStateUrl.length()) {
    return;
  }
  if (lastDeviceStateFetchAttempt != 0 && (now - lastDeviceStateFetchAttempt < DEVICE_STATE_POLL_MS)) {
    return;
  }
  lastDeviceStateFetchAttempt = now;

  HTTPClient http;
  WiFiClient client;
  http.begin(client, deviceStateUrl);
  if (cfg.bearerToken[0]) {
    http.addHeader("Authorization", String("Bearer ") + cfg.bearerToken);
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  const bool suppressInitialDueBeeps = !deviceStateLoaded;
  uint64_t nowEpochMs = 0;
  bool seenButton[BUTTON_COUNT] = {false, false, false, false};

  int lineStart = 0;
  while (lineStart < body.length()) {
    int lineEnd = body.indexOf('\n', lineStart);
    if (lineEnd < 0) {
      lineEnd = body.length();
    }

    String line = body.substring(lineStart, lineEnd);
    line.trim();
    if (line.length() > 0) {
      int c1 = line.indexOf(',');
      if (c1 > 0) {
        String head = line.substring(0, c1);
        if (head == "now") {
          nowEpochMs = parseUint64(line.substring(c1 + 1));
        } else {
          int c2 = line.indexOf(',', c1 + 1);
          int c3 = c2 > 0 ? line.indexOf(',', c2 + 1) : -1;
          int c4 = c3 > 0 ? line.indexOf(',', c3 + 1) : -1;
          if (c2 > 0 && c3 > 0 && c4 > 0) {
            const int button = head.toInt();
            if (button >= 1 && button <= static_cast<int>(BUTTON_COUNT)) {
              const uint8_t idx = static_cast<uint8_t>(button - 1);
              const uint64_t timestampMs = parseUint64(line.substring(c1 + 1, c2));
              const int mutedInt = line.substring(c2 + 1, c3).toInt();
              const unsigned long alertAfterMs = static_cast<unsigned long>(parseUint64(line.substring(c3 + 1, c4)));
              const unsigned long alertIntervalMs = static_cast<unsigned long>(parseUint64(line.substring(c4 + 1)));

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
          }
        }
      }
    }

    lineStart = lineEnd + 1;
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
  actionQueue = xQueueCreate(ACTION_QUEUE_LEN, sizeof(TimerAction));
  toneQueue = xQueueCreate(TONE_QUEUE_LEN, sizeof(TonePattern));
  uint64_t mac = ESP.getEfuseMac();
  char suffixBuf[7];
  snprintf(suffixBuf, sizeof(suffixBuf), "%06llX", mac & 0xFFFFFFULL);
  suffix = String(suffixBuf);
  char deviceBuf[13];
  snprintf(deviceBuf, sizeof(deviceBuf), "%012llX", mac & 0xFFFFFFFFFFFFULL);
  deviceId = String(deviceBuf);

  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT);
  }

  enqueueStartupToneSeries();

  loadConfig();
  startPortal();
  if (actionQueue != nullptr) {
    xTaskCreate(
      timerActionTask,
      "timer-action",
      8192,
      nullptr,
      1,
      nullptr
    );
  } else {
    setStatus("API queue init failed");
  }

  beginWiFiConnect();
}

void loop() {
  unsigned long now = millis();

  configServer.handleClient();
  if (wifiReconnectRequested && (long)(now - wifiReconnectAt) >= 0) {
    wifiReconnectRequested = false;
    beginWiFiConnect();
  }
  updateButtons();
  pollDeviceState(now);
  now = millis();
  updateLocalAlerts(now);
  flushLoadedTimerToneQueue(now);
  updateTone(now);
  updateWiFiConnection(now);

  if (WiFi.status() != WL_CONNECTED && !wifiConnectInProgress) {
    if (now - lastReconnectAttempt >= WIFI_RETRY_INTERVAL) {
      beginWiFiConnect();
    }
  }

  if (now - lastLedToggle >= LED_TOGGLE_MS) {
    lastLedToggle = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  if (rebootRequested && (long)(now - rebootAt) >= 0) {
    ESP.restart();
  }

  delay(LOOP_DELAY_MS);
}
