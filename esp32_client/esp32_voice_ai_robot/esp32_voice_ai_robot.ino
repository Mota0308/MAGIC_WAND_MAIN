/*
 * ESP32-S3 Voice AI Robot (INMP441 mic + MAX98357A amp)
 *
 * Flow:
 *  - Record N seconds WAV from INMP441 over I2S
 *  - POST WAV to /api/stt -> get text
 *  - POST text to /api/chat -> get reply
 *  - POST reply to /api/tts -> get absolute_url (proxy MP3)
 *  - Download MP3 to RAM -> I2S.playMP3()
 *
 * Controls (Serial Monitor 115200, NL/CRLF ok):
 *  - Type: r  -> toggle record start/stop (stop will send STT->Chat->TTS)
 *
 * Wiring:
 *  - BCLK = GPIO14
 *  - WS/LRCK = GPIO13
 *  - DOUT -> MAX98357A DIN = GPIO11
 *  - DIN  <- INMP441 DOUT  = GPIO12
 *
 * Notes:
 *  - INMP441 VDD must be 3.3V
 *  - MAX98357A VIN can be 5V; tie SD to 3.3V if present
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <ESP_I2S.h>
#include <SD_MMC.h>
#include "esp_heap_caps.h"

// ---------- Feature flags ----------
// Set to 1 to enable TTS audio output; set to 0 to disable speaker playback.
#ifndef ENABLE_TTS
#define ENABLE_TTS 0
#endif

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#define STT_URL   "https://magicwandmain-production.up.railway.app/api/stt"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define TTS_URL   "https://magicwandmain-production.up.railway.app/api/tts"
#define COMMAND_URL "https://magicwandmain-production.up.railway.app/api/command"

#define LOG_URL  "https://magicwandmain-production.up.railway.app/api/log"
#define DEVICE_ID "esp32_voice_001"
// ---------- UDP target (ESP8266) ----------
// Replace with your ESP8266 IP (printed by its UDP server sketch).
#define ESP8266_IP "10.172.186.113"
#define ESP8266_UDP_PORT 4210
#endif

// I2S pins (your setup)
static const int PIN_BCLK = 14;
static const int PIN_WS   = 13;
static const int PIN_DOUT = 11;
static const int PIN_DIN  = 12;

// Audio config
static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BITS_PER_SAMPLE = 16;
static const uint16_t CHANNELS = 1;              // mono
static const uint32_t MAX_REC_SECONDS = 5;       // safety cap (keeps WAV small enough for TLS + STT latency)

static const size_t REC_CHUNK_FRAMES = 256;      // stereo frames (L+R)
static int g_recShift = 16;                      // 32-bit -> 16-bit scaling (tune 10..18)
static bool g_micUseRight = true;                // INMP441 L/R=3.3V usually outputs RIGHT channel

static WiFiUDP g_udp;
static IPAddress g_esp8266Ip;

I2SClass I2S;

static bool parseIp(const char* s, IPAddress& out) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static bool udpSendToEsp8266(const char* cmd) {
  if (!g_esp8266Ip) {
    Serial.println("[UDP] ESP8266 IP not set");
    return false;
  }
  g_udp.beginPacket(g_esp8266Ip, (uint16_t)ESP8266_UDP_PORT);
  g_udp.print(cmd);
  bool ok = g_udp.endPacket();
  Serial.print("[UDP] Sent ");
  Serial.print(cmd);
  Serial.print(" -> ");
  Serial.print(g_esp8266Ip);
  Serial.print(":");
  Serial.println((int)ESP8266_UDP_PORT);
  return ok;
}

static bool matchVoiceCommand(const String& text, String& outCmd, String& outSay) {
  outCmd = "";
  outSay = "";
  String s = text;
  s.toLowerCase();
  s.replace(" ", "");

  // OFF first to avoid "open" vs "close" ambiguity.
  if (s.indexOf("關燈") >= 0 || s.indexOf("關掉") >= 0 || s.indexOf("關") == 0 || s.indexOf("off") >= 0) {
    outCmd = "OFF";
    outSay = "已關閉";
    return true;
  }
  if (s.indexOf("開燈") >= 0 || s.indexOf("打開") >= 0 || s.indexOf("開") == 0 || s.indexOf("on") >= 0) {
    outCmd = "ON";
    outSay = "已開啟";
    return true;
  }
  return false;
}

static bool httpPostCommand(const String& text, String& outDevice, String& outAction, String& outSay) {
  outDevice = "";
  outAction = "";
  outSay = "";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, COMMAND_URL)) return false;
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
    Serial.print("[CMD HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }

  outAction = extractJsonStringField(resp, "action");
  outDevice = extractJsonStringField(resp, "device");
  outSay = extractJsonStringField(resp, "say");
  if (!outAction.length() || !outDevice.length()) {
    Serial.println("[CMD] missing action/device; raw resp:");
    Serial.println(resp);
    return false;
  }
  return true;
}

static bool udpSendDeviceAction(const String& device, const String& action) {
  // Send as: "<device> <action>" e.g. "light1 on"
  String payload = device;
  payload += " ";
  payload += action;
  return udpSendToEsp8266(payload.c_str());
}

static void httpPostLogBestEffort(const String& inputText, const String& outputText, const char* kind) {
  if (!inputText.length() || !outputText.length()) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, LOG_URL)) return;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);
  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"kind\":\"";
  body += kind;
  body += "\",\"input_text\":\"";
  body += escapeJsonString(inputText);
  body += "\",\"output_text\":\"";
  body += escapeJsonString(outputText);
  body += "\"}";
  (void)http.POST(body);
  http.end();
}

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

static void writeWavHeader(uint8_t* dst, uint32_t dataBytes) {
  // PCM WAV header (44 bytes)
  const uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  const uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  const uint32_t riffSize = 36 + dataBytes;

  auto W4 = [&](int off, uint32_t v) {
    dst[off + 0] = (uint8_t)(v & 0xFF);
    dst[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    dst[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    dst[off + 3] = (uint8_t)((v >> 24) & 0xFF);
  };
  auto W2 = [&](int off, uint16_t v) {
    dst[off + 0] = (uint8_t)(v & 0xFF);
    dst[off + 1] = (uint8_t)((v >> 8) & 0xFF);
  };

  memcpy(dst + 0, "RIFF", 4);
  W4(4, riffSize);
  memcpy(dst + 8, "WAVE", 4);
  memcpy(dst + 12, "fmt ", 4);
  W4(16, 16);                 // PCM fmt chunk size
  W2(20, 1);                  // audio format = PCM
  W2(22, CHANNELS);
  W4(24, SAMPLE_RATE);
  W4(28, byteRate);
  W2(32, blockAlign);
  W2(34, BITS_PER_SAMPLE);
  memcpy(dst + 36, "data", 4);
  W4(40, dataBytes);
}

// ---------- Toggle recording (r to start, r to stop) ----------
static bool g_isRecording = false;
static bool g_hasRecording = false;  // recorded data present (ready to send)
static uint32_t g_recStartMs = 0;
static File g_recFile;
static size_t g_recDataBytes = 0;    // payload length (excluding header)
static const char* kRecWavPath = "/rec.wav";
static uint32_t g_lastMeterMs = 0;
static uint32_t g_meterCount = 0;
static uint64_t g_meterAccSq = 0;
static int16_t  g_meterPeak = 0;

static bool i2sBeginForPlayback() {
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, -1);
  // Use 44.1kHz for TTS MP3 playback to preserve clarity (avoid forcing 16kHz output).
  return I2S.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

static bool i2sBeginForRecord() {
  // INMP441 commonly outputs 24-bit audio in 32-bit slots. Use 32-bit stereo to be safe.
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);
  return I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
}

static bool readFileToRam(fs::FS& fs, const char* path, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;
  File f = fs.open(path, FILE_READ);
  if (!f) {
    Serial.println("[SD] open for read failed");
    return false;
  }
  size_t len = (size_t)f.size();
  if (len < 16) {
    Serial.println("[SD] file too small");
    f.close();
    return false;
  }
  Serial.print("[SD] file size=");
  Serial.println(len);
  Serial.print("[SD] free heap before malloc=");
  Serial.println(ESP.getFreeHeap());
  Serial.print("[SD] largest free internal block=");
  Serial.println(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#if CONFIG_SPIRAM_SUPPORT
  Serial.print("[SD] largest free PSRAM block=");
  Serial.println(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif

  // Prefer PSRAM for large MP3 buffers to avoid internal-heap fragmentation issues.
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(len);
  if (!buf) {
    Serial.println("[ERR] malloc failed (MP3)");
    Serial.print("[SD] free heap at malloc fail=");
    Serial.println(ESP.getFreeHeap());
    f.close();
    return false;
  }
  size_t got = f.read(buf, len);
  f.close();
  if (got != len) {
    Serial.println("[SD] short read");
    free(buf);
    return false;
  }
  *outBuf = buf;
  *outLen = len;
  return true;
}

static bool sdInit() {
  static bool inited = false;
  if (inited) return true;
  // Freenove ESP32-S3 WROOM onboard SD uses SDMMC 1-bit:
  // CMD=38, CLK=39, D0=40 (per Freenove docs).
  SD_MMC.setPins(39, 38, 40);
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("[SD] SD_MMC.begin failed");
    return false;
  }
  uint64_t cardSize = SD_MMC.cardSize();
  Serial.print("[SD] OK, cardSize=");
  Serial.println((unsigned long)(cardSize / (1024 * 1024)));
  inited = true;
  return true;
}

static void recFree() {
  if (g_recFile) g_recFile.close();
  g_recDataBytes = 0;
  g_isRecording = false;
  g_hasRecording = false;
  g_lastMeterMs = 0;
  g_meterCount = 0;
  g_meterAccSq = 0;
  g_meterPeak = 0;
}

static bool recStart() {
  recFree();
  if (!sdInit()) return false;
  if (!i2sBeginForRecord()) {
    Serial.println("[ERR] I2S begin for record failed");
    return false;
  }

  if (SD_MMC.exists(kRecWavPath)) SD_MMC.remove(kRecWavPath);
  g_recFile = SD_MMC.open(kRecWavPath, FILE_WRITE);
  if (!g_recFile) {
    Serial.println("[SD] open rec.wav for write failed");
    return false;
  }
  // placeholder header
  uint8_t hdr[44];
  writeWavHeader(hdr, 0);
  g_recFile.write(hdr, sizeof(hdr));
  g_recDataBytes = 0;

  g_recStartMs = millis();
  g_isRecording = true;
  g_hasRecording = false;
  g_lastMeterMs = millis();
  return true;
}

static void recPump() {
  if (!g_isRecording || !g_recFile) return;

  // Read 32-bit stereo frames: [L, R, L, R, ...]
  int32_t buf32[REC_CHUNK_FRAMES * 2];
  size_t got = I2S.readBytes((char*)buf32, sizeof(buf32));
  if (!got) return;

  // Convert to mono int16 while writing to WAV.
  size_t framesGot = got / (sizeof(int32_t) * 2);
  const size_t samplesToWrite = framesGot;
  int16_t out16[REC_CHUNK_FRAMES];
  for (size_t i = 0; i < samplesToWrite; i++) {
    int32_t l = buf32[i * 2 + 0];
    int32_t r = buf32[i * 2 + 1];
    // INMP441 typically drives only one channel depending on L/R pin.
    // Averaging L+R can reduce SNR if the other channel is mostly noise/zeros.
    int32_t s32 = g_micUseRight ? r : l;
    // scale down; clamp
    int32_t v = s32 >> g_recShift;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    int16_t s = (int16_t)v;
    out16[i] = s;

    // meter
    int32_t a = s < 0 ? -s : s;
    if (a > g_meterPeak) g_meterPeak = (int16_t)a;
    g_meterAccSq += (uint64_t)((int32_t)s * (int32_t)s);
    g_meterCount++;
  }
  size_t wantBytes16 = samplesToWrite * sizeof(int16_t);
  size_t w = g_recFile.write((uint8_t*)out16, wantBytes16);
  g_recDataBytes += w;

  // Print meter every ~200ms to help tune g_recShift (avoid clipping/too quiet)
  uint32_t now = millis();
  if (now - g_lastMeterMs >= 200 && g_meterCount > 0) {
    uint32_t meanSq = (uint32_t)(g_meterAccSq / g_meterCount);
    // integer sqrt approx is fine for debugging; use float to keep it simple
    float rms = sqrtf((float)meanSq);
    Serial.print("[REC METER] shift=");
    Serial.print(g_recShift);
    Serial.print(" peak=");
    Serial.print((int)g_meterPeak);
    Serial.print(" rms=");
    Serial.println(rms, 1);
    g_lastMeterMs = now;
    g_meterCount = 0;
    g_meterAccSq = 0;
    g_meterPeak = 0;
  }

  // Safety cap: keep WAV small enough for reliable TLS + STT latency.
  if (MAX_REC_SECONDS > 0 && (now - g_recStartMs) >= (MAX_REC_SECONDS * 1000UL)) {
    size_t totalBytes = 0;
    (void)recStopAndFinalizeSd(totalBytes);
    Serial.print("[REC] AUTO STOP (max ");
    Serial.print(MAX_REC_SECONDS);
    Serial.println("s)");
    Serial.print("[OK] wav bytes=");
    Serial.println(totalBytes);
    Serial.println("[REC] Stopped. Type 'r' again to SEND.");
  }
}

static bool recStopAndFinalizeSd(size_t& outBytes) {
  outBytes = 0;
  if (!g_recFile) return false;
  g_isRecording = false;
  g_hasRecording = true;

  // rewrite header with final size
  uint8_t hdr[44];
  writeWavHeader(hdr, (uint32_t)g_recDataBytes);
  g_recFile.seek(0);
  g_recFile.write(hdr, sizeof(hdr));
  g_recFile.flush();
  g_recFile.close();
  outBytes = 44 + g_recDataBytes;
  return true;
}

static bool httpPostWavForStt(const uint8_t* wav, size_t wavLen, String& outText) {
  outText = "";
  Serial.print("[STT] free heap before POST=");
  Serial.println(ESP.getFreeHeap());
  Serial.print("[STT] bytes=");
  Serial.println(wavLen);

  // Retry because Railway/Poe can be momentarily unavailable, and low heap can fail TLS.
  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(30);
    // Note: Some Arduino-ESP32 builds don't expose TLS buffer tuning APIs on WiFiClientSecure.

    HTTPClient http;
    if (!http.begin(client, STT_URL)) {
      Serial.println("[STT] http.begin failed");
      delay(400);
      continue;
    }
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(180000);

    int code = http.POST((uint8_t*)wav, wavLen);
    String resp = http.getString();
    http.end();

    if (code == 200) {
      outText = extractJsonStringField(resp, "text");
      if (!outText.length()) {
        Serial.println("[STT] empty text; raw resp:");
        Serial.println(resp);
        return false;
      }
      return true;
    }

    Serial.print("[STT HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.print("[STT] ");
    Serial.println(HTTPClient::errorToString(code));
    Serial.print("[STT] attempt ");
    Serial.print(attempt);
    Serial.print("/3, free heap=");
    Serial.println(ESP.getFreeHeap());

    // If connection refused, backoff and retry.
    delay(600 * attempt);
  }

  return false;
}

static bool httpPostWavFileForStt(const char* wavPath, String& outText) {
  outText = "";
  if (!sdInit()) return false;
  File f = SD_MMC.open(wavPath, FILE_READ);
  if (!f) {
    Serial.println("[SD] open wav for STT failed");
    return false;
  }
  const size_t wavLen = (size_t)f.size();
  Serial.print("[STT] wav file bytes=");
  Serial.println(wavLen);
  Serial.print("[STT] free heap before POST=");
  Serial.println(ESP.getFreeHeap());

  // Retry: Railway/Poe can be momentarily unavailable.
  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(60);
    client.setTimeout(300000); // ms; affects read timeouts

    HTTPClient http;
    if (!http.begin(client, STT_URL)) {
      Serial.println("[STT] http.begin failed");
      delay(400);
      continue;
    }
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(300000);

    f.seek(0);
    int code = http.sendRequest("POST", &f, wavLen);
    String resp = http.getString();
    http.end();

    if (code == 200) {
      outText = extractJsonStringField(resp, "text");
      if (!outText.length()) {
        Serial.println("[STT] empty text; raw resp:");
        Serial.println(resp);
        f.close();
        return false;
      }
      f.close();
      return true;
    }

    Serial.print("[STT HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(HTTPClient::errorToString(code));
    Serial.println(resp);
    Serial.print("[STT] attempt ");
    Serial.print(attempt);
    Serial.print("/3, free heap=");
    Serial.println(ESP.getFreeHeap());
    delay(900 * attempt);
  }

  f.close();
  return false;
}

static bool httpPostChat(const String& text, String& outReply) {
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
  body += escapeJsonString(text);
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

static bool httpPostTts(const String& text, String& outAbsoluteUrl) {
  outAbsoluteUrl = "";
  Serial.print("[TTS] free heap before POST=");
  Serial.println(ESP.getFreeHeap());

  // Retry: TTS can be slow; network may timeout.
  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(30);

    HTTPClient http;
    if (!http.begin(client, TTS_URL)) {
      Serial.println("[TTS] http.begin failed");
      delay(500 * attempt);
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(180000); // Poe TTS may take > 60s

    String body = "{\"device_id\":\"";
    body += DEVICE_ID;
    body += "\",\"text\":\"";
    body += escapeJsonString(text);
    body += "\"}";

    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (code == 200) {
      outAbsoluteUrl = extractJsonStringField(resp, "absolute_url");
      if (!outAbsoluteUrl.length()) {
        Serial.println("[TTS] missing absolute_url; raw resp:");
        Serial.println(resp);
        return false;
      }
      return true;
    }

    Serial.print("[TTS HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(HTTPClient::errorToString(code));
    Serial.println(resp);
    Serial.print("[TTS] attempt ");
    Serial.print(attempt);
    Serial.print("/3, free heap=");
    Serial.println(ESP.getFreeHeap());
    delay(700 * attempt);
  }

  return false;
}

static bool downloadToSdFile(const char* url, const char* path, size_t& outSize) {
  outSize = 0;
  if (!sdInit()) return false;

  // Remove old file if present
  if (SD_MMC.exists(path)) SD_MMC.remove(path);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] open for write failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[ERR] http.begin failed");
    f.close();
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.print("[ERR] HTTP ");
    Serial.println(code);
    Serial.println(http.getString());
    http.end();
    f.close();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[2048];
  unsigned long start = millis();
  while (http.connected()) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail;
      if (toRead > sizeof(buf)) toRead = sizeof(buf);
      int r = stream->readBytes(buf, toRead);
      if (r > 0) {
        size_t w = f.write(buf, (size_t)r);
        outSize += w;
        start = millis();
      }
    } else {
      if (millis() - start > 20000) break;
      delay(5);
    }
  }
  http.end();
  f.close();
  Serial.print("[SD] wrote bytes=");
  Serial.println(outSize);
  return outSize > 0;
}

static bool speakViaTts(const String& text) {
#if !ENABLE_TTS
  (void)text;
  return true;
#else
  // Release recorder I2S before handing I2S to Audio library.
  I2S.end();
  delay(50);

  String mp3Url;
  if (!httpPostTts(text, mp3Url)) return false;
  Serial.print("[TTS] url=");
  Serial.println(mp3Url);

  const char* kMp3Path = "/tts.mp3";
  size_t mp3Bytes = 0;
  Serial.print("[TTS] downloading to SD ");
  Serial.println(kMp3Path);
  if (!downloadToSdFile(mp3Url.c_str(), kMp3Path, mp3Bytes)) {
    Serial.println("[ERR] MP3 download to SD failed");
    return false;
  }

  // Read MP3 from SD into RAM, then play via ESP_I2S (known-good on this board).
  uint8_t* mp3 = nullptr;
  size_t mp3Len = 0;
  if (!readFileToRam(SD_MMC, kMp3Path, &mp3, &mp3Len)) {
    Serial.println("[ERR] read MP3 to RAM failed");
    return false;
  }

  if (!i2sBeginForPlayback()) {
    Serial.println("[ERR] I2S begin for playback failed");
    free(mp3);
    i2sBeginForRecord();
    return false;
  }

  Serial.println("[SAY] playing from RAM via ESP_I2S...");
  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("playMP3 ok=");
  Serial.println(ok ? "true" : "false");
  free(mp3);

  // Re-init mic I2S for next recording.
  i2sBeginForRecord();
  return ok;
#endif
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== ESP32 Voice AI Robot ===");
  Serial.println("Type 'r' then Enter to START recording; type 'r' again to STOP and send.");

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

  if (!parseIp(ESP8266_IP, g_esp8266Ip)) {
    Serial.println("[UDP] ESP8266_IP invalid; set it in wifi_secrets.h or in this sketch");
  } else {
    g_udp.begin(0); // any local port
    Serial.print("[UDP] target ESP8266=");
    Serial.print(g_esp8266Ip);
    Serial.print(":");
    Serial.println((int)ESP8266_UDP_PORT);
  }

  if (!i2sBeginForRecord()) {
    Serial.println("[ERR] I2S begin for record failed");
    while (true) delay(1000);
  }
  Serial.print("[OK] I2S ready (mic). TTS=");
  Serial.println(ENABLE_TTS ? "ON" : "OFF");
}

void loop() {
  // keep recording in background
  if (g_isRecording) {
    recPump();
  }

  if (!Serial.available()) {
    delay(5);
    return;
  }

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.startsWith("shift")) {
    int v = cmd.substring(5).toInt();
    if (v >= 8 && v <= 20) {
      g_recShift = v;
      Serial.print("[CFG] g_recShift=");
      Serial.println(g_recShift);
    } else {
      Serial.println("[CFG] usage: shift10..shift16");
    }
    return;
  }
  if (cmd == "micL" || cmd == "micl") {
    g_micUseRight = false;
    Serial.println("[CFG] mic channel = LEFT");
    return;
  }
  if (cmd == "micR" || cmd == "micr") {
    g_micUseRight = true;
    Serial.println("[CFG] mic channel = RIGHT");
    return;
  }
  if (cmd != "r") return;

  // If we have a finished recording (auto-stopped or user-stopped), pressing r will SEND it.
  if (!g_isRecording && g_hasRecording) {
    Serial.println("[REC] SEND");
    String userText;
    Serial.println("[STT] Sending...");
    bool sttOk = httpPostWavFileForStt(kRecWavPath, userText);
    recFree();
    if (!sttOk) return;
    Serial.print("[YOU] ");
    Serial.println(userText);

    // If STT text matches a device-control command, send it to ESP8266 via UDP.
    String cmdUdp, say;
    if (matchVoiceCommand(userText, cmdUdp, say)) {
      Serial.print("[CMD] ");
      Serial.println(cmdUdp);
      udpSendToEsp8266(cmdUdp.c_str());
      httpPostLogBestEffort(userText, cmdUdp, "command");
      // TTS output is optional (controlled by ENABLE_TTS).
#if ENABLE_TTS
      if (say.length()) {
        Serial.println("[SAY] Speaking...");
        speakViaTts(say);
      }
#endif
      Serial.println("[DONE] Type 'r' to record again.");
      return;
    }

    String reply;
    Serial.println("[CHAT] Asking...");
    if (!httpPostChat(userText, reply)) return;
    Serial.print("[AI] ");
    Serial.println(reply);
    httpPostLogBestEffort(userText, reply, "chat");

#if ENABLE_TTS
    Serial.println("[SAY] Speaking...");
    speakViaTts(reply);
#endif
    Serial.println("[DONE] Type 'r' to record again.");
    return;
  }

  if (!g_isRecording) {
    Serial.println("[REC] START (type 'r' again to stop) ...");
    if (!recStart()) {
      Serial.println("[ERR] record start failed (OOM?)");
      return;
    }
    return;
  }

  Serial.println("[REC] STOP");
  // Stop now; user must press 'r' once more to send (so they can stop, then decide to send).
  size_t wavLenTmp = 0;
  if (!recStopAndFinalizeSd(wavLenTmp)) {
    Serial.println("[ERR] record stop failed");
    recFree();
    return;
  }
  Serial.print("[OK] wav bytes=");
  Serial.println(wavLenTmp);
  Serial.println("[REC] Stopped. Type 'r' again to SEND.");
}

