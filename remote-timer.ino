#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>

// Buzzer output pin (adjust for your ESP32-C3 wiring).
constexpr uint8_t BUZZER_PIN = 10;
constexpr uint8_t BUZZER_CHANNEL = 0;
constexpr uint16_t BUTTON_TONES[4] = {523, 659, 784, 988}; // C5, E5, G5, B5
constexpr unsigned long TONE_MS = 90;

// YK04 receiver outputs (adjust pins to match your board wiring).
constexpr uint8_t BUTTON_PINS[4] = {2, 3, 4, 5};
constexpr unsigned long DEBOUNCE_MS = 100;
constexpr unsigned long LONG_PRESS_MS = 1200;

// Wi-Fi and config portal.
constexpr unsigned long WIFI_CONNECT_TIMEOUT = 15000UL;
constexpr unsigned long WIFI_RETRY_INTERVAL = 20000UL;

constexpr size_t SSID_MAX_LEN = 32;
constexpr size_t PASS_MAX_LEN = 64;
constexpr size_t URL_MAX_LEN = 128;
constexpr size_t TOKEN_MAX_LEN = 128;

struct DeviceConfig {
  char ssid[SSID_MAX_LEN + 1] = {};
  char password[PASS_MAX_LEN + 1] = {};
  char webhookUrl[URL_MAX_LEN + 1] = "http://192.168.1.101:30109/webhook/timer";
  char bearerToken[TOKEN_MAX_LEN + 1] = {};
};

struct ButtonState {
  bool stablePressed = false;
  bool lastRawPressed = false;
  bool longTriggered = false;
  unsigned long rawChangedAt = 0;
  unsigned long pressedAt = 0;
};

Preferences prefs;
DeviceConfig cfg;
WebServer configServer(80);
ButtonState buttons[4];

unsigned long lastReconnectAttempt = 0;

String statusLine = "Booting";
unsigned long statusUpdatedAt = 0;

String apSsid;
String deviceId;

static void setStatus(const String& s) {
  statusLine = s;
  statusUpdatedAt = millis();
  Serial.println(s);
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

void loadConfig() {
  prefs.begin("remote-timer", true);

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String webhook = prefs.getString("api_url", cfg.webhookUrl);
  String token = prefs.getString("api_token", "");

  prefs.end();

  ssid.toCharArray(cfg.ssid, sizeof(cfg.ssid));
  pass.toCharArray(cfg.password, sizeof(cfg.password));
  webhook.toCharArray(cfg.webhookUrl, sizeof(cfg.webhookUrl));
  token.toCharArray(cfg.bearerToken, sizeof(cfg.bearerToken));
}

void saveConfig() {
  prefs.begin("remote-timer", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.password);
  prefs.putString("api_url", cfg.webhookUrl);
  prefs.putString("api_token", cfg.bearerToken);
  prefs.end();
}

bool connectWiFi() {
  if (!cfg.ssid[0]) {
    setStatus("No WiFi SSID configured");
    return false;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.ssid, cfg.password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setStatus("WiFi connected: " + WiFi.localIP().toString());
    return true;
  }

  setStatus("WiFi connect failed");
  return false;
}

String portalPage() {
  String html;
  html.reserve(2600);
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
  html += statusLine;
  html += "</p>";

  html += "<form method='POST' action='/save'>";
  html += "<h2>WiFi</h2>";
  html += "<label>SSID</label><input name='ssid' maxlength='32' value='" + String(cfg.ssid) + "'>";
  html += "<label>Password</label><input type='password' name='password' maxlength='64' value='" + String(cfg.password) + "'>";

  html += "<h2>API</h2>";
  html += "<label>Timer Webhook URL</label><input name='api_url' maxlength='128' value='" + String(cfg.webhookUrl) + "'>";
  html += "<label>Bearer Token (optional)</label><input type='password' name='api_token' maxlength='128' value='" + String(cfg.bearerToken) + "'>";
  html += "<small>Short press: POST .../webhook/timer?id=device:button</small><br>";
  html += "<small>Long press: DELETE .../webhook/timer?id=device:button</small><br>";
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
  copyFormField(configServer.arg("api_url"), cfg.webhookUrl, sizeof(cfg.webhookUrl));
  copyFormField(configServer.arg("api_token"), cfg.bearerToken, sizeof(cfg.bearerToken));

  if (!cfg.webhookUrl[0]) {
    configServer.send(400, "text/html", "<h2>Invalid API config</h2><a href='/'>Back</a>");
    return;
  }

  saveConfig();
  bool ok = connectWiFi();

  String body = "<h2>Saved</h2><p>WiFi reconnect: ";
  body += ok ? "OK" : "Failed";
  body += "</p><a href='/'>Back</a>";
  configServer.send(200, "text/html", body);
}

void startPortal() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06llX", mac & 0xFFFFFFULL);
  apSsid = String("RemoteTimer-") + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str());

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.onNotFound(handleRoot);
  configServer.begin();

  setStatus("Config AP: " + apSsid);
}

bool postTimerAction(uint8_t buttonIndex, const char* action) {
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("API skipped: no WiFi");
    return false;
  }

  String idValue = deviceId + ":" + String(buttonIndex + 1);
  String url = appendIdQuery(String(cfg.webhookUrl), idValue);
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
  String resp = code > 0 ? http.getString() : http.errorToString(code);
  http.end();

  if (code >= 200 && code < 300) {
    setStatus(String(action) + " btn " + String(buttonIndex + 1) + " OK (" + String(code) + ")");
    return true;
  }

  setStatus(String(action) + " btn " + String(buttonIndex + 1) + " fail (" + String(code) + ") " + resp);
  return false;
}

void playButtonTone(uint8_t buttonIndex) {
  if (buttonIndex >= 4) {
    return;
  }
  ledcWriteTone(BUZZER_CHANNEL, BUTTON_TONES[buttonIndex]);
  delay(TONE_MS);
  ledcWriteTone(BUZZER_CHANNEL, 0);
}

void handleButtonAction(uint8_t i, bool isLongPress) {
  if (isLongPress) {
    postTimerAction(i, "delete");
  } else {
    postTimerAction(i, "restart");
  }
}

void updateButtons() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < 4; i++) {
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
        playButtonTone(i);
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
  Serial.begin(115200);
  delay(100);
  Serial.println("=== ESP32-C3 Remote Timer Boot ===");
  char deviceBuf[13];
  snprintf(deviceBuf, sizeof(deviceBuf), "%012llX", ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL);
  deviceId = String(deviceBuf);
  ledcSetup(BUZZER_CHANNEL, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(BUTTON_PINS[i], INPUT);
  }

  loadConfig();
  startPortal();

  connectWiFi();
}

void loop() {
  configServer.handleClient();
  updateButtons();

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= WIFI_RETRY_INTERVAL) {
      lastReconnectAttempt = now;
      connectWiFi();
    }
  }

  delay(50);
}
