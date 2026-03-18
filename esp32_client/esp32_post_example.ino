/*
 * ESP32 → Railway (Python + MongoDB) POST 範例
 *
 * 功能：連 WiFi，將 AI 辨識結果以 JSON POST 到雲端，並依回傳的 feedback 做輸出。
 *
 * 必要程式庫：ArduinoJson（程式庫管理員搜尋 "ArduinoJson" 安裝）
 * 開發板：ESP32 Dev Module（或你使用的 ESP32 型號）
 *
 * 請修改下方 WIFI_SSID、WIFI_PASS、API_URL。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------- 請改成你的設定 --------
const char* WIFI_SSID     = "your_wifi_ssid";
const char* WIFI_PASS     = "your_wifi_password";
const char* API_URL       = "https://your-app.railway.app/api/data";  // Railway 部署後的網址
const char* DEVICE_ID     = "esp32_001";
// --------------------------------

// 範例：用 LED 表示雲端回傳的 action（ESP32 內建 LED 常見為 GPIO 2）
const int LED_PIN = 2;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi 連線中");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi 已連線");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // 範例：每 10 秒送一筆資料（實作時可改為「AI 辨識到結果才送」）
  static unsigned long lastSend = 0;
  if (millis() - lastSend < 10000) {
    delay(100);
    return;
  }
  lastSend = millis();

  // -------- 組裝要 POST 的 JSON（與 Railway API 約定一致）--------
  // 格式：{ "device_id", "label", "score", "sensor", "timestamp", "extra" }
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["label"]     = "3";       // 實際請填 AI 辨識結果，例如 "0"~"9"
  doc["score"]     = 85;        // 信心分數
  doc["sensor"]    = "gesture";
  doc["timestamp"] = (long)(millis() / 1000);

  String body;
  serializeJson(doc, body);
  Serial.println("POST body: " + body);

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);

  if (code == 201 || code == 200) {
    String resp = http.getString();
    Serial.println("Response: " + resp);

    // 解析回傳的 feedback（action / message）
    StaticJsonDocument<256> respDoc;
    if (deserializeJson(respDoc, resp) == DeserializationError::Ok) {
      const char* action = respDoc["feedback"]["action"] | "none";
      const char* msg    = respDoc["feedback"]["message"] | "";

      Serial.print("feedback.action = ");
      Serial.println(action);
      Serial.print("feedback.message = ");
      Serial.println(msg);

      // 依雲端回傳的 action 控制輸出
      if (strcmp(action, "led_on") == 0) {
        digitalWrite(LED_PIN, HIGH);
      } else if (strcmp(action, "led_off") == 0) {
        digitalWrite(LED_PIN, LOW);
      }
    }
  } else {
    Serial.print("HTTP error ");
    Serial.println(code);
    Serial.println(http.getString());
  }
  http.end();
}
