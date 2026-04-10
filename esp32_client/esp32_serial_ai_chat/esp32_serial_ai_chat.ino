/*
 * 文字版 AI 機器人：Serial 輸入一行 → POST 雲端 /api/chat → Serial 印出回覆
 *
 * 設定方式（擇一）：
 *   1) 複製 wifi_secrets.example.h 為 wifi_secrets.h 並填入 WiFi / CHAT_URL
 *   2) 若沒有 wifi_secrets.h，則使用下方預設常數
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <IRremote.hpp>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID     "GAN"
#define WIFI_PASS     "chen0605"
#define CHAT_URL      "https://magicwandmain-production.up.railway.app/api/chat"
#define DEVICE_ID     "esp32_serial_001"
#endif

// ---------------- IR transmitter (test) ----------------
// Note: GPIO5 must be a valid output pin on your ESP32 board.
// If IR doesn't work, try another GPIO (e.g. 2/4/18) and rewire.
#define IR_SEND_PIN 5
static const unsigned long IR_SEND_INTERVAL_MS = 300;
static unsigned long lastIrSendMs = 0;
// -------------------------------------------------------

static String escapeJsonString(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"' || c == '\n' || c == '\r') {
      if (c == '\n') { o += "\\n"; continue; }
      if (c == '\r') continue;
      o += '\\';
    }
    o += c;
  }
  return o;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// 將 Unicode codepoint 轉成 UTF-8 加到 out
static void appendUtf8(String& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += (char)cp;
  } else if (cp <= 0x7FF) {
    out += (char)(0xC0 | ((cp >> 6) & 0x1F));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += (char)(0xE0 | ((cp >> 12) & 0x0F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | ((cp >> 18) & 0x07));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

// 解析 JSON 字串內的跳脫：\\n \\t \\\" \\\\ 以及 \\uXXXX（含 surrogate pair）
static bool consumeJsonEscape(const String& json, unsigned& i, String& out) {
  if (i >= json.length()) return false;
  char n = json[i];
  if (n == 'n') { out += '\n'; i++; return true; }
  if (n == 't') { out += '\t'; i++; return true; }
  if (n == 'r') { /* ignore */ i++; return true; }
  if (n == '"' ) { out += '"'; i++; return true; }
  if (n == '\\') { out += '\\'; i++; return true; }
  if (n != 'u') { out += n; i++; return true; }

  // \\uXXXX
  if (i + 4 >= json.length()) return false;
  uint32_t u1 = 0;
  for (int k = 0; k < 4; k++) {
    int v = hexVal(json[i + 1 + k]);
    if (v < 0) return false;
    u1 = (u1 << 4) | (uint32_t)v;
  }
  i += 5; // consume 'u' + 4 hex

  // surrogate pair?
  if (u1 >= 0xD800 && u1 <= 0xDBFF) {
    if (i + 5 < json.length() && json[i] == '\\' && json[i + 1] == 'u') {
      uint32_t u2 = 0;
      for (int k = 0; k < 4; k++) {
        int v = hexVal(json[i + 2 + k]);
        if (v < 0) { appendUtf8(out, u1); return true; }
        u2 = (u2 << 4) | (uint32_t)v;
      }
      if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
        uint32_t cp = 0x10000 + (((u1 - 0xD800) << 10) | (u2 - 0xDC00));
        appendUtf8(out, cp);
        i += 6; // consume \\uXXXX
        return true;
      }
    }
  }

  appendUtf8(out, u1);
  return true;
}

// 從 JSON 取出 "reply":"..."（支援內含跳脫字元）
static String extractReplyField(const String& json) {
  int k = json.indexOf("\"reply\"");
  if (k < 0) return "";
  int colon = json.indexOf(':', k);
  if (colon < 0) return "";
  int q = json.indexOf('"', colon + 1);
  if (q < 0) return "";
  String out;
  for (unsigned i = (unsigned)q + 1; i < json.length(); ) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.length()) {
      i++; // move to escape type
      if (!consumeJsonEscape(json, i, out)) break;
      continue;
    }
    if (c == '"') break;
    out += c;
    i++;
  }
  return out;
}

static String extractErrorField(const String& json) {
  int k = json.indexOf("\"error\"");
  if (k < 0) return "";
  int colon = json.indexOf(':', k);
  int q1 = json.indexOf('"', colon + 1);
  int q2 = json.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32 Serial AI Chat ===");
  Serial.println("連 WiFi 後，輸入一行文字並按 Enter 送到雲端。");
  Serial.println();

  IrSender.begin(IR_SEND_PIN);
  Serial.print("IR transmitter ready. PIN=");
  Serial.println(IR_SEND_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // 來自 wifi_secrets.h 或上方預設
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("已連線，IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("請輸入：");
}

void loop() {
  // IR: send every second (non-blocking)
  unsigned long now = millis();
  if (now - lastIrSendMs >= IR_SEND_INTERVAL_MS) {
    lastIrSendMs = now;
    IrSender.sendNEC(0x00FF, 0x01, 0);
    Serial.println("Sent IR signal");
  }

  if (!Serial.available()) {
    delay(50);
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.length() > 2000) {
    Serial.println("[錯誤] 單次請勿超過 2000 字元");
    return;
  }

  String body = "{\"device_id\":\"";
  body += String(DEVICE_ID);
  body += "\",\"message\":\"";
  body += escapeJsonString(line);
  body += "\"}";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, CHAT_URL)) {
    Serial.println("[錯誤] http.begin 失敗");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(120000);
  int code = http.POST(body);

  String resp = http.getString();
  http.end();

  if (code == 200) {
    String reply = extractReplyField(resp);
    if (reply.length() == 0) {
      Serial.println("[回應] ");
      Serial.println(resp);
    } else {
      Serial.println("----- AI 回覆 -----");
      Serial.println(reply);
      Serial.println("-------------------");
    }
  } else {
    Serial.print("[HTTP ");
    Serial.print(code);
    Serial.println("]");
    String err = extractErrorField(resp);
    if (err.length()) Serial.println(err);
    Serial.println(resp);
  }
  Serial.println();
  Serial.println("請繼續輸入：");
}
