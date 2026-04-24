#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#define LED_PIN 8
unsigned long lastLedToggle = 0;
bool ledState = false;

// Buzzer output pin (adjust for your ESP32-C3 wiring).
constexpr uint8_t BUZZER_PIN = 10;
constexpr uint16_t SHORT_PRESS_TONES[4] = {392, 494, 587, 698}; // G4, B4, D5, F5
constexpr uint16_t LONG_PRESS_TONES[4] = {523, 659, 784, 988};  // C5, E5, G5, B5
constexpr unsigned long TONE_MS = 90;
constexpr unsigned long TONE_GAP_MS = 70;

// YK04 receiver outputs (adjust pins to match your board wiring).
constexpr uint8_t BUTTON_PINS[4] = {0, 1, 3, 4};
constexpr uint8_t BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);
constexpr unsigned long DEBOUNCE_MS = 100;
constexpr unsigned long LONG_PRESS_MS = 3000;

// Wi-Fi and config portal.
constexpr unsigned long WIFI_CONNECT_TIMEOUT = 15000UL;
constexpr unsigned long WIFI_RETRY_INTERVAL = 20000UL;

constexpr size_t SSID_MAX_LEN = 32;
constexpr size_t PASS_MAX_LEN = 64;
constexpr size_t HOST_MAX_LEN = 128;
constexpr size_t TOKEN_MAX_LEN = 128;
constexpr uint8_t ACTION_QUEUE_LEN = 8;
constexpr uint8_t TONE_QUEUE_LEN = 16;
constexpr unsigned long LED_TOGGLE_MS = 1000UL;
constexpr unsigned long LOOP_DELAY_MS = 1UL;

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

String suffix;
bool toneOutputActive = false;
bool tonePatternActive = false;
TonePattern currentTonePattern;
unsigned long toneStageEndsAt = 0;

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

static String appendIdQuery(const String& baseUrl, const String& idValue) {
  String url = baseUrl;
  if (url.indexOf('?') >= 0) {
    url += "&id=" + idValue;
  } else {
    url += "?id=" + idValue;
  }
  return url;
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

static String hostFromWebhookUrl(const String& webhookUrl) {
  String url = webhookUrl;
  url.trim();
  int start = 0;
  if (url.startsWith("http://")) {
    start = 7;
  } else if (url.startsWith("https://")) {
    start = 8;
  }

  int slash = url.indexOf('/', start);
  if (slash < 0) {
    slash = url.length();
  }
  return url.substring(start, slash);
}

static String webhookUrlFromHost(const String& apiHost) {
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
  return host + "/api/timer";
}

static void refreshWebhookBase() {
  webhookBase = webhookUrlFromHost(String(cfg.apiHost));
}

void loadConfig() {
  prefs.begin("remote-timer", true);

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String host = prefs.getString("api_host", "");
  String legacyWebhook = prefs.getString("api_url", "");
  String token = prefs.getString("api_token", "");

  prefs.end();

  if (!host.length() && legacyWebhook.length()) {
    host = hostFromWebhookUrl(legacyWebhook);
  }
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
  prefs.putString("api_url", webhookBase);
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
    setStatus("WiFi connected: " + WiFi.localIP().toString());
    return;
  }

  if (now - wifiConnectStartedAt >= WIFI_CONNECT_TIMEOUT) {
    wifiConnectInProgress = false;
    WiFi.disconnect();
    setStatus("WiFi connect failed");
  }
}

String portalPage() {
  String html;
  html.reserve(2600);
  String escapedStatus = htmlEscape(getStatusLine());
  String escapedSsid = htmlEscape(String(cfg.ssid));
  String escapedPassword = htmlEscape(String(cfg.password));
  String escapedApiHost = htmlEscape(String(cfg.apiHost));
  String escapedBearerToken = htmlEscape(String(cfg.bearerToken));
  String escapedWebhookBase = htmlEscape(webhookBase);

  html += "<!doctype html><html><head><meta charset='utf-8'><title>Remote Timer Config</title>";
  html += "<style>body{font-family:sans-serif;max-width:760px;margin:1.5rem auto;padding:0 1rem;}";
  html += "label{display:block;margin-top:.8rem;font-weight:600}input{width:100%;padding:.5rem;}";
  html += "button{margin-top:1rem;padding:.6rem 1rem;}small{color:#555}</style></head><body>";
  html += "<h1>ESP32-C3 Remote Timer</h1>";

  if (WiFi.status() == WL_CONNECTED) {
    html += "<p><b>WiFi:</b> Connected to ";
    html += WiFi.SSID();
    html += " (";
    html += WiFi.localIP().toString();
    html += ")</p>";
  } else {
    html += "<p><b>WiFi:</b> Not connected</p>";
  }

  html += "<p><b>Status:</b> ";
  html += escapedStatus;
  html += "</p>";

  html += "<form method='POST' action='/save'>";
  html += "<h2>WiFi</h2>";
  html += "<label>SSID</label><input name='ssid' maxlength='32' value='" + escapedSsid + "'>";
  html += "<label>Password</label><input type='password' name='password' maxlength='64' value='" + escapedPassword + "'>";

  html += "<h2>API</h2>";
  html += "<label>API Host (host:port)</label><input name='api_host' maxlength='128' value='" + escapedApiHost + "'>";
  html += "<label>Bearer Token (optional)</label><input type='password' name='api_token' maxlength='128' value='" + escapedBearerToken + "'>";
  html += "<small>Short press: POST " + escapedWebhookBase + "?id=suffix-button (e.g. " + suffix + "-1)</small><br>";
  html += "<small>Long press: DELETE " + escapedWebhookBase + "?id=suffix-button (e.g. " + suffix + "-1)</small><br>";
  html += "<small>This device ID: ";
  html += deviceId;
  html += "</small><br>";

  html += "<button type='submit'>Save & Reconnect</button></form></body></html>";
  return html;
}

void handleRoot() {
  configServer.send(200, "text/html", portalPage());
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
  beginWiFiConnect();

  String body = "<h2>Saved</h2><p>WiFi reconnect started. Check status on the main page.</p><a href='/'>Back</a>";
  configServer.send(200, "text/html", body);
}

void startPortal() {
  
  apSsid = String("RemoteTimer-") + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str());

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.onNotFound(handleRoot);
  configServer.begin();

  setStatus("Config AP: " + apSsid);
}

bool performTimerAction(uint8_t buttonIndex, const char* action) {
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("API skipped: no WiFi");
    return false;
  }

  String idValue = suffix + "-" + String(buttonIndex + 1);
  if (!webhookBase.length()) {
    setStatus("API skipped: invalid host");
    return false;
  }
  String url = appendIdQuery(webhookBase, idValue);
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
    setStatus(String(action) + " btn " + String(buttonIndex + 1) + " OK (" + String(code) + ")");
    return true;
  }

  String resp = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  setStatus(String(action) + " btn " + String(buttonIndex + 1) + " fail (" + String(code) + ") " + resp);
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

  setStatus(String(isDelete ? "delete" : "set") + " btn " + String(buttonIndex + 1) + " queued");
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

  return xQueueSend(toneQueue, &pattern, 0) == pdTRUE;
}

void timerActionTask(void* parameter) {
  TimerAction action;

  for (;;) {
    if (xQueueReceive(actionQueue, &action, portMAX_DELAY) == pdTRUE) {
      if (performTimerAction(action.buttonIndex, action.isDelete ? "delete" : "set")) {
        enqueueTonePattern(action.buttonIndex, action.isDelete, 2);
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

    if (rawPressed != b.stablePressed) {
      b.stablePressed = rawPressed;

      if (b.stablePressed) {
        b.pressedAt = now;
        b.longTriggered = false;
      } else {
        if (!b.longTriggered) {
          handleButtonAction(i, false);
        }
      }
    }

    if (b.stablePressed && !b.longTriggered && (now - b.pressedAt >= LONG_PRESS_MS)) {
      b.longTriggered = true;
      handleButtonAction(i, true);
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
  updateButtons();
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

  delay(LOOP_DELAY_MS);
}
