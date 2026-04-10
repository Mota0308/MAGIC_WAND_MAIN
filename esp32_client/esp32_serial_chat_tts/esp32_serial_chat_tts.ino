/*
 * ESP32 Serial Chat + TTS (text in -> AI reply text out + TTS audio out)
 *
 * Flow:
 *  - Serial input line
 *  - POST /api/chat -> get "reply"
 *  - POST /api/tts  -> get "absolute_url" (proxy MP3)
 *  - Download MP3 to RAM -> play via ESP_I2S.playMP3()
 *
 * This avoids on-device STT. If you later want voice input, we can add cloud STT.
 *
 * Wiring (MAX98357A):
 *  - BCLK -> GPIO14
 *  - LRC/WS -> GPIO13
 *  - DIN -> GPIO11
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define TTS_URL   "https://magicwandmain-production.up.railway.app/api/tts"
#define DEVICE_ID "esp32_serial_001"
#endif

static const int PIN_BCLK = 14;
static const int PIN_WS   = 13;
static const int PIN_DOUT = 11;

I2SClass I2S;

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

static String extractJsonStringField(const String& json, const char* field) {
  String key = "\"";
  key += field;
  key += "\"";
  int k = json.indexOf(key);
  if (k < 0) return "";
  int colon = json.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return "";

  String out;
  for (unsigned i = (unsigned)q1 + 1; i < json.length(); ) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.length()) {
      char n = json[i + 1];
      if (n == 'n') { out += '\n'; i += 2; continue; }
      if (n == 't') { out += '\t'; i += 2; continue; }
      if (n == 'r') { i += 2; continue; }
      out += n;
      i += 2;
      continue;
    }
    if (c == '"') break;
    out += c;
    i++;
  }
  return out;
}

static uint8_t* downloadToRam(const char* url, size_t& outSize) {
  outSize = 0;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[ERR] http.begin failed");
    return nullptr;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.print("[ERR] HTTP ");
    Serial.println(code);
    Serial.println(http.getString());
    http.end();
    return nullptr;
  }

  int len = http.getSize();
  const size_t kMax = 512 * 1024;
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (len > 0) ? (size_t)len : (64 * 1024);
  if (cap > kMax) cap = kMax;

  uint8_t* buf = (uint8_t*)malloc(cap);
  if (!buf) {
    Serial.println("[ERR] malloc failed");
    http.end();
    return nullptr;
  }

  size_t pos = 0;
  unsigned long start = millis();
  while (http.connected()) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail;
      if (pos + toRead > cap) {
        size_t newCap = cap * 2;
        if (newCap < pos + toRead) newCap = pos + toRead;
        if (newCap > kMax) {
          Serial.println("[ERR] file too large");
          free(buf);
          http.end();
          return nullptr;
        }
        uint8_t* nb = (uint8_t*)realloc(buf, newCap);
        if (!nb) {
          Serial.println("[ERR] realloc failed");
          free(buf);
          http.end();
          return nullptr;
        }
        buf = nb;
        cap = newCap;
      }
      int r = stream->readBytes(buf + pos, toRead);
      if (r > 0) pos += (size_t)r;
    } else {
      if (millis() - start > 20000) break;
      delay(5);
    }
    if (len > 0 && (int)pos >= len) break;
  }
  http.end();
  outSize = pos;
  return buf;
}

static bool postChat(const String& userText, String& outReply) {
  outReply = "";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, CHAT_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(120000);

  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"message\":\"";
  body += escapeJsonString(userText);
  body += "\"}";

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.print("[CHAT HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }

  outReply = extractJsonStringField(resp, "reply");
  if (!outReply.length()) {
    Serial.println("[CHAT] missing reply; raw resp:");
    Serial.println(resp);
    return false;
  }
  return true;
}

static bool postTts(const String& text, String& outAbsoluteUrl) {
  outAbsoluteUrl = "";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, TTS_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(120000);

  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"text\":\"";
  body += escapeJsonString(text);
  body += "\"}";

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.print("[TTS HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }

  outAbsoluteUrl = extractJsonStringField(resp, "absolute_url");
  if (!outAbsoluteUrl.length()) {
    Serial.println("[TTS] missing absolute_url; raw resp:");
    Serial.println(resp);
    return false;
  }
  return true;
}

static bool playTtsFromText(const String& text) {
  String url;
  if (!postTts(text, url)) return false;
  Serial.print("[TTS] url=");
  Serial.println(url);

  size_t mp3Len = 0;
  uint8_t* mp3 = downloadToRam(url.c_str(), mp3Len);
  if (!mp3 || mp3Len < 16) {
    Serial.println("[ERR] mp3 download failed");
    if (mp3) free(mp3);
    return false;
  }
  Serial.print("[OK] mp3 bytes=");
  Serial.println(mp3Len);

  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("playMP3 ok=");
  Serial.println(ok ? "true" : "false");
  free(mp3);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== ESP32 Serial Chat + TTS (ESP_I2S) ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi OK. IP: ");
  Serial.println(WiFi.localIP());

  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, -1);
  if (!I2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[ERR] I2S.begin failed");
    while (true) delay(1000);
  }

  Serial.println("輸入一行文字後按 Enter：");
}

void loop() {
  if (!Serial.available()) {
    delay(30);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;

  String reply;
  Serial.println("[CHAT] asking...");
  if (!postChat(line, reply)) {
    Serial.println("輸入下一行：");
    return;
  }

  Serial.println("----- AI 回覆 -----");
  Serial.println(reply);
  Serial.println("-------------------");

  Serial.println("[SAY] playing TTS...");
  playTtsFromText(reply);

  Serial.println("輸入下一行：");
}

