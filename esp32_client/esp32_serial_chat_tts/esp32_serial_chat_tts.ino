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
#include "esp_heap_caps.h"

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

static String extractJsonStringField(const String& json, const char* field) {
  String key = "\"";
  key += field;
  key += "\"";
  int k = json.indexOf(key);
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

static uint8_t* bufAlloc(size_t sz, bool& useSpiRam) {
  uint8_t* p = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    useSpiRam = true;
    return p;
  }
  useSpiRam = false;
  return (uint8_t*)malloc(sz);
}

static uint8_t* bufRealloc(uint8_t* p, size_t newSz, bool useSpiRam) {
  if (useSpiRam) {
    return (uint8_t*)heap_caps_realloc(p, newSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return (uint8_t*)realloc(p, newSz);
}

static void bufFree(uint8_t* p, bool useSpiRam) {
  if (!p) return;
  if (useSpiRam) {
    heap_caps_free(p);
  } else {
    free(p);
  }
}

// outSpiRam: 若非 nullptr，回傳緩衝區是否位於 PSRAM（釋放時需對應 bufFree）
static uint8_t* downloadToRam(const char* url, size_t& outSize, bool* outSpiRam = nullptr) {
  outSize = 0;
  if (outSpiRam) *outSpiRam = false;
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
  // 不要一次 malloc 整個 Content-Length（內部 SRAM 常無大連續區塊）；優先 PSRAM
  const size_t kFirstChunk = 64 * 1024;
  size_t cap = (len > 0) ? (size_t)len : kFirstChunk;
  if (cap > kMax) cap = kMax;
  if (len > 0 && cap > kFirstChunk) cap = kFirstChunk;

  bool useSpiRam = false;
  uint8_t* buf = bufAlloc(cap, useSpiRam);
  if (!buf) {
    Serial.println("[ERR] malloc failed");
    Serial.print("[HEAP] free=");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" largest=");
    Serial.println(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
          bufFree(buf, useSpiRam);
          http.end();
          return nullptr;
        }
        uint8_t* nb = bufRealloc(buf, newCap, useSpiRam);
        if (!nb) {
          Serial.println("[ERR] realloc failed");
          bufFree(buf, useSpiRam);
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
  if (outSpiRam) *outSpiRam = useSpiRam;
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
  bool mp3Spi = false;
  uint8_t* mp3 = downloadToRam(url.c_str(), mp3Len, &mp3Spi);
  if (!mp3 || mp3Len < 16) {
    Serial.println("[ERR] mp3 download failed");
    if (mp3) bufFree(mp3, mp3Spi);
    return false;
  }
  Serial.print("[OK] mp3 bytes=");
  Serial.println(mp3Len);

  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("playMP3 ok=");
  Serial.println(ok ? "true" : "false");
  bufFree(mp3, mp3Spi);
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

