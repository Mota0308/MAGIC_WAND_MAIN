/*
 * ESP32 / ESP32-S3 → Railway POST 範例（不需 ArduinoJson）
 *
 * 請修改 WIFI_SSID、WIFI_PASS、API_URL。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -------- 請改成你的設定 --------
const char* WIFI_SSID     = "GAN";
const char* WIFI_PASS     = "chen0605";
const char* API_URL       = "https://magicwandmain-production.up.railway.app/api/data";
const char* DEVICE_ID     = "esp32_001";
// --------------------------------

const int LED_PIN = 2;

// 從 JSON 裡取出 "action":"xxx" 的值
static bool parseJsonStringField(const String& json, const char* key, String& out) {
  String pat = String("\"") + key + String("\"");
  int i = json.indexOf(pat);
  if (i < 0) return false;
  int colon = json.indexOf(':', i + pat.length());
  if (colon < 0) return false;
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  out = json.substring(q1 + 1, q2);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi OK");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend < 10000) {
    delay(100);
    return;
  }
  lastSend = millis();

  // 手動組 JSON（與 Railway /api/data 一致）
  String body = "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"label\":\"3\",";
  body += "\"score\":85,";
  body += "\"sensor\":\"gesture\",";
  body += "\"timestamp\":" + String((long)(millis() / 1000));
  body += "}";
  Serial.println("POST: " + body);

  WiFiClientSecure client;
  client.setInsecure();  // Railway HTTPS；正式環境可改為驗證憑證

  HTTPClient http;
  if (!http.begin(client, API_URL)) {
    Serial.println("http.begin failed");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code == 201 || code == 200) {
    String resp = http.getString();
    Serial.println("Response: " + resp);
    String action;
    if (parseJsonStringField(resp, "action", action)) {
      Serial.println("feedback.action = " + action);
      if (action == "led_on") {
        digitalWrite(LED_PIN, HIGH);
      } else if (action == "led_off") {
        digitalWrite(LED_PIN, LOW);
      }
    }
  } else {
    Serial.print("HTTP ");
    Serial.println(code);
    Serial.println(http.getString());
  }
  http.end();
}
