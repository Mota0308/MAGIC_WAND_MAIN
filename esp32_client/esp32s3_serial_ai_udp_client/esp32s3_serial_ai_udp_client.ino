/*
 * ESP32-S3 — Serial 文字 或 I2S 麥克風 -> STT (/api/stt) -> AI (/api/chat) + UDP 控制 ESP32-C3 LED
 *
 * 流程：
 *   1) Serial 讀一行
 *      - 若整行為 r（不分大小寫）-> 開始 I2S 錄音；再輸入一行 r -> 停止錄音並 POST /api/stt -> 與文字指令相同流程
 *      - 否則視為文字指令（與先前相同）
 *   2) 若 SERIAL_AI_FORCE_CLOUD=0 且符合本地關鍵字 -> UDP（不經雲端）
 *   3) 否則 POST /api/chat；依 device_cmd 送 UDP
 *
 * C3 端請燒錄：esp32c3_udp_led_server.ino，並把本機 UDP_TARGET_IP 設為 C3 的 IP。
 *
 * 雲端 /api/chat 會回傳 JSON 欄位 device_cmd: ON | OFF | NONE | B0..B100。
 * 設 SERIAL_AI_FORCE_CLOUD 為 1 可略過本地關鍵字、一律走雲端 AI。
 *
 * --- INMP441（I2S 麥克風模組）與 ESP32-S3 建議接線（與 esp32_voice_ai_robot / esp32_i2s_audio_test 一致）---
 *   INMP441          ESP32-S3
 *   VDD  -> 3.3V   （勿接 5V）
 *   GND  -> GND
 *   WS   -> GPIO13  (LRCK)
 *   SCK  -> GPIO14  (BCLK)
 *   SD   -> GPIO12  (ESP DIN / 麥克風 DOUT)
 *   L/R  -> GND 或 3.3V（選左/右聲道）；執行時也可用 Serial 指令 micL / micR 切換（與 esp32_voice_ai_robot 一致）
 * 若同時接 MAX98357A 喇叭：DIN -> GPIO11，與麥克風共用 BCLK/WS（本程式僅錄音時使用 I2S）。
 *
 * Serial 115200；ESP32-S3 若 Serial 空白可開 Tools -> USB CDC On Boot
 */

#include <Arduino.h>
#include <string.h>

#ifndef SERIAL_AI_FORCE_CLOUD
#define SERIAL_AI_FORCE_CLOUD 1  // 1 = 全部交給雲端 AI 判斷 device_cmd
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>

#ifndef ENABLE_I2S_MIC
#define ENABLE_I2S_MIC 1  // 0 = 不使用麥克風，略過 r 錄音與 ESP_I2S
#endif
#ifndef MIC_MAX_RECORD_SECONDS
// 與 esp32_voice_ai_robot 的 MAX_REC_SECONDS 一致（安全上限）；無 PSRAM 時分配會自動降秒數
#define MIC_MAX_RECORD_SECONDS 5
#endif

#if ENABLE_I2S_MIC
#include <ESP_I2S.h>
#include "esp_heap_caps.h"
#endif

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#define STT_URL   "https://magicwandmain-production.up.railway.app/api/stt"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define LOG_URL  "https://magicwandmain-production.up.railway.app/api/log"
#define DEVICE_ID "esp32s3_ai_client_001"
#define UDP_TARGET_IP   "10.232.188.113"
#define UDP_TARGET_PORT 4210
#endif

#if ENABLE_I2S_MIC && !defined(STT_URL)
#define STT_URL "https://magicwandmain-production.up.railway.app/api/stt"
#endif

#if ENABLE_I2S_MIC
// I2S 腳位（與 esp32_voice_ai_robot.ino 相同：INMP441 + 可選 MAX98357A）
static const int PIN_BCLK = 14;
static const int PIN_WS = 13;
static const int PIN_DOUT = 11;  // -> MAX98357A DIN（僅麥克風時可不接）
static const int PIN_DIN = 12;   // <- INMP441 SD

// 音訊（與 esp32_voice_ai_robot：SAMPLE_RATE / BITS / CHANNELS / REC_CHUNK_FRAMES）
static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BITS_PER_SAMPLE = 16;
static const uint16_t CHANNELS = 1;
static const size_t REC_CHUNK_FRAMES = 256;
static int g_recShift = 16;       // 32-bit I2S -> int16 右移，可用 shift8..shift20 調整（同 voice robot）
static bool g_micUseRight = true; // 可用 micL / micR 切換（同 voice robot）

static I2SClass I2S;

static bool g_recording = false;
static uint8_t* g_wavBuf = nullptr;
static size_t g_wavCap = 0;
static uint32_t g_pcmDataBytes = 0;
static uint32_t g_recStartMs = 0;
static uint32_t g_recMaxMs = 0;
static String g_serialLineBuf;
#endif

static WiFiUDP g_udp;
static IPAddress g_targetIp;

static bool parseIp(const char* s, IPAddress& out) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static bool udpSendCommand(const char* cmd) {
  if (!g_targetIp) {
    Serial.println("[UDP] target IP invalid — set UDP_TARGET_IP in wifi_secrets.h");
    return false;
  }
  g_udp.beginPacket(g_targetIp, (uint16_t)UDP_TARGET_PORT);
  g_udp.print(cmd);
  bool ok = g_udp.endPacket();
  Serial.print("[UDP] ");
  Serial.print(cmd);
  Serial.print(" -> ");
  Serial.print(g_targetIp);
  Serial.print(":");
  Serial.println(UDP_TARGET_PORT);
  return ok;
}

static bool matchVoiceCommand(const String& text, String& outCmd, String& outSay) {
  outCmd = "";
  outSay = "";
  String s = text;
  s.toLowerCase();
  s.replace(" ", "");

  // 避免把「r」當成開燈關鍵字（與錄音啟停衝突）
  if (s == "r") return false;

  // 亮度（與 ESP32-C3 UDP B0/B25/... 一致；需先於 ON/OFF 以免被「開燈」誤判）
  if (s.indexOf("100%") >= 0 || s.indexOf("最亮") >= 0 || s.indexOf("全亮") >= 0) {
    outCmd = "B100";
    outSay = "已設為最亮";
    return true;
  }
  if (s.indexOf("75%") >= 0 || s.indexOf("七成五") >= 0) {
    outCmd = "B75";
    outSay = "已設為 75%";
    return true;
  }
  if (s.indexOf("50%") >= 0 || s.indexOf("一半") >= 0 || s.indexOf("五成") >= 0) {
    outCmd = "B50";
    outSay = "已設為 50%";
    return true;
  }
  if (s.indexOf("25%") >= 0 || s.indexOf("二成五") >= 0) {
    outCmd = "B25";
    outSay = "已設為 25%";
    return true;
  }
  if (s.indexOf("最暗") >= 0 || s.indexOf("全暗") >= 0) {
    outCmd = "B0";
    outSay = "已設為最暗";
    return true;
  }

  // OFF first (avoid ambiguity with phrases containing 開/關)
  if (s.indexOf("關燈") >= 0 || s.indexOf("關掉") >= 0 || s.indexOf("關") == 0 || s.indexOf("off") >= 0) {
    outCmd = "OFF";
    outSay = "已關閉";
    return true;
  }
  // ON: 中文口語 + 英文（與 esp32_serial_ai_robot 一致，並補常見說法）
  if (s.indexOf("開燈") >= 0 || s.indexOf("打開") >= 0 || s.indexOf("開") == 0 || s.indexOf("on") >= 0 ||
      s.indexOf("幫我打開") >= 0 || s.indexOf("幫開") >= 0 || s.indexOf("點亮") >= 0 || s.indexOf("點燈") >= 0 ||
      s.indexOf("亮燈") >= 0 || s.indexOf("turnon") >= 0) {
    outCmd = "ON";
    outSay = "已開啟";
    return true;
  }
  return false;
}

static String escapeJsonString(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"' || c == '\n' || c == '\r') {
      if (c == '\n') {
        o += "\\n";
        continue;
      }
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

static bool consumeJsonEscape(const String& json, unsigned& i, String& out) {
  if (i >= json.length()) return false;
  char n = json[i];
  if (n == 'n') {
    out += '\n';
    i++;
    return true;
  }
  if (n == 't') {
    out += '\t';
    i++;
    return true;
  }
  if (n == 'r') {
    i++;
    return true;
  }
  if (n == '"') {
    out += '"';
    i++;
    return true;
  }
  if (n == '\\') {
    out += '\\';
    i++;
    return true;
  }
  if (n != 'u') {
    out += n;
    i++;
    return true;
  }
  if (i + 4 >= json.length()) return false;
  uint32_t u1 = 0;
  for (int k = 0; k < 4; k++) {
    int v = hexVal(json[i + 1 + k]);
    if (v < 0) return false;
    u1 = (u1 << 4) | (uint32_t)v;
  }
  i += 5;
  if (u1 >= 0xD800 && u1 <= 0xDBFF) {
    if (i + 5 < json.length() && json[i] == '\\' && json[i + 1] == 'u') {
      uint32_t u2 = 0;
      for (int k = 0; k < 4; k++) {
        int v = hexVal(json[i + 2 + k]);
        if (v < 0) {
          appendUtf8(out, u1);
          return true;
        }
        u2 = (u2 << 4) | (uint32_t)v;
      }
      if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
        uint32_t cp = 0x10000 + (((u1 - 0xD800) << 10) | (u2 - 0xDC00));
        appendUtf8(out, cp);
        i += 6;
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
  for (unsigned i = (unsigned)q + 1; i < json.length();) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.length()) {
      i++;
      if (!consumeJsonEscape(json, i, out)) break;
      continue;
    }
    if (c == '"') break;
    out += c;
    i++;
  }
  return out;
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

static bool httpPostChat(const String& text, String& outReply, String& outDeviceCmd) {
  outReply = "";
  outDeviceCmd = "";
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
  outDeviceCmd = extractJsonStringField(resp, "device_cmd");
  if (!outReply.length()) {
    Serial.println("[CHAT] missing reply");
    Serial.println(resp);
    return false;
  }
  return true;
}

#if ENABLE_I2S_MIC
static void writeWavHeader(uint8_t* dst, uint32_t dataBytes) {
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
  W4(16, 16);
  W2(20, 1);
  W2(22, CHANNELS);
  W4(24, SAMPLE_RATE);
  W4(28, byteRate);
  W2(32, blockAlign);
  W2(34, BITS_PER_SAMPLE);
  memcpy(dst + 36, "data", 4);
  W4(40, dataBytes);
}

static bool i2sBeginForMic() {
  // INMP441 多為 32-bit slot；與 esp32_voice_ai_robot::i2sBeginForRecord 相同
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);
  return I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
}

static void recFreeAbort() {
  g_recording = false;
  I2S.end();
  if (g_wavBuf) {
    free(g_wavBuf);
    g_wavBuf = nullptr;
  }
  g_wavCap = 0;
  g_pcmDataBytes = 0;
  g_recMaxMs = 0;
}

/** 優先 PSRAM，其次內部連續塊，最後 malloc（降低錄音緩衝 malloc 失敗機率） */
static uint8_t* allocRecordingBuffer(size_t cap) {
  uint8_t* p = nullptr;
  if (ESP.getPsramSize() > 0) {
    p = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!p) {
    p = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_8BIT);
  }
  if (!p) {
    p = (uint8_t*)malloc(cap);
  }
  return p;
}

static void printMallocHint(size_t needBytes) {
  Serial.print("[REC] malloc 失敗，需要約 ");
  Serial.print((unsigned)needBytes);
  Serial.println(" bytes（WAV PCM 緩衝）");
  Serial.print("[REC] freeHeap=");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" largestBlock=");
  Serial.print((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (ESP.getPsramSize() > 0) {
    Serial.print(" psramFree=");
    Serial.print((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  Serial.println();
  Serial.println("[REC] 可縮小 MIC_MAX_RECORD_SECONDS，或啟用板載 PSRAM（Arduino 選項）");
}

static bool recTryStart() {
  if (g_recording) {
    Serial.println("[REC] 已在錄音中");
    return false;
  }

  int wantSec = MIC_MAX_RECORD_SECONDS;
  if (wantSec < 1) wantSec = 1;
  if (wantSec > 60) wantSec = 60;

  g_wavBuf = nullptr;
  g_wavCap = 0;
  int gotSec = 0;
  for (int sec = wantSec; sec >= 1; sec--) {
    uint32_t maxSamples = SAMPLE_RATE * (uint32_t)sec;
    size_t dataMax = (size_t)maxSamples * sizeof(int16_t);
    size_t cap = 44 + dataMax;
    uint8_t* buf = allocRecordingBuffer(cap);
    if (buf) {
      g_wavBuf = buf;
      g_wavCap = cap;
      gotSec = sec;
      if (sec != wantSec) {
        Serial.print("[REC] 記憶體不足，已自動改為最長 ");
        Serial.print(sec);
        Serial.println(" 秒");
      }
      break;
    }
  }

  if (!g_wavBuf) {
    size_t need1 = 44 + (size_t)SAMPLE_RATE * sizeof(int16_t);
    printMallocHint(need1);
    return false;
  }

  g_recMaxMs = (uint32_t)gotSec * 1000UL;
  g_pcmDataBytes = 0;
  writeWavHeader(g_wavBuf, 0);
  if (!i2sBeginForMic()) {
    Serial.println("[REC] I2S 開始失敗");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    return false;
  }
  delay(200);
  g_recording = true;
  g_recStartMs = millis();
  g_serialLineBuf = "";
  Serial.println("[REC] 錄音中… 再輸入 r + Enter 停止並上傳（或達最長時間自動停止）");
  return true;
}

static void recPump() {
  if (!g_recording || !g_wavBuf) return;
  size_t maxPcm = g_wavCap - 44;
  if (g_pcmDataBytes >= maxPcm) return;

  int32_t tmp[REC_CHUNK_FRAMES * 2];
  size_t spacePcm = maxPcm - g_pcmDataBytes;
  size_t maxSamples = spacePcm / sizeof(int16_t);
  size_t chunk = maxSamples < REC_CHUNK_FRAMES ? maxSamples : REC_CHUNK_FRAMES;
  if (chunk == 0) return;

  size_t got = I2S.readBytes((char*)tmp, chunk * sizeof(int32_t) * 2);
  if (!got) return;

  int16_t* dst = (int16_t*)(g_wavBuf + 44 + g_pcmDataBytes);
  size_t frames = got / (sizeof(int32_t) * 2);
  size_t outN = 0;
  for (size_t i = 0; i < frames; i++) {
    if (g_pcmDataBytes + (outN + 1) * sizeof(int16_t) > maxPcm) break;
    int32_t l = tmp[i * 2 + 0];
    int32_t r = tmp[i * 2 + 1];
    int32_t s32 = g_micUseRight ? r : l;
    int32_t v = s32 >> g_recShift;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    dst[outN++] = (int16_t)v;
  }
  g_pcmDataBytes += outN * sizeof(int16_t);
}

/** 錄音中呼叫：非阻塞讀 Serial，若收到單獨一行 r 則回傳 true */
static bool pollSerialRecordingStop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String t = g_serialLineBuf;
      g_serialLineBuf = "";
      t.trim();
      if (t.length() == 1 && (t[0] == 'r' || t[0] == 'R')) return true;
      if (t.length() > 0) {
        Serial.println("[REC] 停止請只輸入 r（已忽略此行）");
      }
      continue;
    }
    if (g_serialLineBuf.length() < 32) g_serialLineBuf += c;
  }
  return false;
}

static bool httpPostStt(const uint8_t* wav, size_t wavLen, String& outText) {
  outText = "";
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  if (!http.begin(client, STT_URL)) return false;
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(180000);
  int code = http.POST((uint8_t*)wav, wavLen);
  String resp = http.getString();
  http.end();
  if (code != 200) {
    Serial.print("[STT HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }
  outText = extractJsonStringField(resp, "text");
  return outText.length() > 0;
}

static void processUserTextLine(const String& line);

static void micFinishAndUpload() {
  g_recording = false;
  g_recMaxMs = 0;
  I2S.end();
  if (!g_wavBuf) {
    g_pcmDataBytes = 0;
    return;
  }
  const uint32_t minBytes = (uint32_t)(SAMPLE_RATE / 2) * (uint32_t)sizeof(int16_t);
  if (g_pcmDataBytes < minBytes) {
    Serial.println("[REC] 錄音太短，已取消");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    g_pcmDataBytes = 0;
    Serial.println("請繼續輸入：");
    return;
  }
  writeWavHeader(g_wavBuf, g_pcmDataBytes);
  const size_t wavLen = 44 + (size_t)g_pcmDataBytes;
  String said;
  if (!httpPostStt(g_wavBuf, wavLen, said)) {
    Serial.println("[REC] STT 失敗（檢查 STT_URL、WiFi、後端金鑰）");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    g_pcmDataBytes = 0;
    Serial.println("請繼續輸入：");
    return;
  }
  free(g_wavBuf);
  g_wavBuf = nullptr;
  g_wavCap = 0;
  g_pcmDataBytes = 0;
  said.trim();
  if (said.length() == 0) {
    Serial.println("[REC] 辨識為空");
    Serial.println("請繼續輸入：");
    return;
  }
  Serial.print("[YOU voice] ");
  Serial.println(said);
  processUserTextLine(said);
}
#endif  // ENABLE_I2S_MIC

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== ESP32-S3 AI + UDP -> ESP32-C3 LED ===");
#if SERIAL_AI_FORCE_CLOUD
  Serial.println("Mode: cloud-only (SERIAL_AI_FORCE_CLOUD=1) -> device_cmd from /api/chat");
#else
  Serial.println("Mode: local keywords first; else /api/chat -> JSON device_cmd (ON/OFF/NONE)");
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi OK  IP: ");
  Serial.println(WiFi.localIP());

  if (!parseIp(UDP_TARGET_IP, g_targetIp)) {
    Serial.println("[UDP] Invalid UDP_TARGET_IP");
  } else {
    g_udp.begin(0);
    Serial.print("[UDP] target ESP32-C3 ");
    Serial.print(g_targetIp);
    Serial.print(":");
    Serial.println(UDP_TARGET_PORT);
  }

  Serial.println();
  Serial.println("輸入一行文字；輸入 r 開始錄音，再輸入 r 停止並上傳語音：");
#if ENABLE_I2S_MIC
  Serial.println("麥克風調校（同 esp32_voice_ai_robot）: micL / micR 聲道, shift8..shift20 增益");
#endif
}

static void processUserTextLine(const String& line) {
  String cmdUdp, say;
#if !SERIAL_AI_FORCE_CLOUD
  if (matchVoiceCommand(line, cmdUdp, say)) {
    Serial.print("[PATH] local keyword -> UDP ");
    Serial.println(cmdUdp);
    udpSendCommand(cmdUdp.c_str());
    httpPostLogBestEffort(line, cmdUdp, "command");
    if (say.length()) Serial.println(say);
    Serial.println("[DONE]\n請繼續輸入：");
    return;
  }
#endif

  String reply;
  String deviceCmd;
  Serial.println("[CHAT] ...");
  if (!httpPostChat(line, reply, deviceCmd)) {
    Serial.println("請繼續輸入：");
    return;
  }
  Serial.print("[AI] ");
  Serial.println(reply);
  deviceCmd.toLowerCase();
  deviceCmd.trim();
  Serial.print("[CLOUD] device_cmd=");
  Serial.println(deviceCmd.length() ? deviceCmd : "(empty)");

  httpPostLogBestEffort(line, reply + " |cmd:" + deviceCmd, "chat");

  if (deviceCmd == "on") {
    Serial.println("[PATH] AI device_cmd -> UDP ON");
    udpSendCommand("ON");
    httpPostLogBestEffort(line, "ON", "ai_device_cmd");
  } else if (deviceCmd == "off") {
    Serial.println("[PATH] AI device_cmd -> UDP OFF");
    udpSendCommand("OFF");
    httpPostLogBestEffort(line, "OFF", "ai_device_cmd");
  } else if (deviceCmd == "b0" || deviceCmd == "b25" || deviceCmd == "b50" || deviceCmd == "b75" ||
             deviceCmd == "b100") {
    String u = deviceCmd;
    u.toUpperCase();
    Serial.print("[PATH] AI device_cmd -> UDP ");
    Serial.println(u);
    udpSendCommand(u.c_str());
    httpPostLogBestEffort(line, u, "ai_device_cmd");
  } else if (matchVoiceCommand(reply, cmdUdp, say)) {
    Serial.print("[PATH] AI reply text fallback -> UDP ");
    Serial.println(cmdUdp);
    udpSendCommand(cmdUdp.c_str());
    httpPostLogBestEffort(reply, cmdUdp, "ai_udp");
  }

  Serial.println("[DONE]\n請繼續輸入：");
}

void loop() {
#if ENABLE_I2S_MIC
  if (g_recording) {
    recPump();
    if (g_wavBuf && g_pcmDataBytes >= g_wavCap - 44) {
      Serial.println("[REC] 緩衝已滿，停止並上傳");
      micFinishAndUpload();
      return;
    }
    if (g_recMaxMs > 0 && millis() - g_recStartMs >= g_recMaxMs) {
      Serial.println("[REC] 已達最長錄音時間，自動停止並上傳");
      micFinishAndUpload();
      return;
    }
    if (pollSerialRecordingStop()) {
      Serial.println("[REC] 停止，上傳 STT…");
      micFinishAndUpload();
      return;
    }
    delay(1);
    return;
  }
#endif

  if (!Serial.available()) {
    delay(10);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.length() > 2000) {
    Serial.println("[ERR] 超過 2000 字元");
    return;
  }

#if ENABLE_I2S_MIC
  if (line.startsWith("shift")) {
    int v = line.substring(5).toInt();
    if (v >= 8 && v <= 20) {
      g_recShift = v;
      Serial.print("[CFG] g_recShift=");
      Serial.println(g_recShift);
    } else {
      Serial.println("[CFG] 用法: shift8 .. shift20（與 esp32_voice_ai_robot）");
    }
    return;
  }
  if (line == "micL" || line == "micl") {
    g_micUseRight = false;
    Serial.println("[CFG] mic channel = LEFT");
    return;
  }
  if (line == "micR" || line == "micr") {
    g_micUseRight = true;
    Serial.println("[CFG] mic channel = RIGHT");
    return;
  }
#endif

#if ENABLE_I2S_MIC
  {
    String key = line;
    key.toLowerCase();
    if (key == "r") {
      if (!recTryStart()) {
        Serial.println("請繼續輸入：");
      }
      return;
    }
  }
#endif

  Serial.print("[YOU] ");
  Serial.println(line);
  processUserTextLine(line);
}
