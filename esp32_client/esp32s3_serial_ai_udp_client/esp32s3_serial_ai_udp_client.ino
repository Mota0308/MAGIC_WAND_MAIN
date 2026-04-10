/*
 * ESP32-S3 — Serial 文字 或 I2S 麥克風 -> STT (/api/stt) -> AI (/api/chat) + UDP 控制 ESP32-C3 LED
 *
 * 流程：
 *   1) Serial 讀一行
 *      - 若整行為 r（不分大小寫）-> 開始 I2S 錄音；再輸入一行 r -> 停止錄音並 POST /api/stt -> 與文字指令相同流程
 *      - 若以 udp 開頭 -> 僅管理目標清單（add/use/list），不經 AI
 *      - 其餘一律 POST /api/chat，**僅依 JSON device_cmd / device_target** 送 UDP（無本地關鍵字判燈）
 *
 * C3 端請燒錄：esp32c3_udp_led_server.ino；預設目標可在 wifi_secrets.h 設 UDP_TARGET_IP，或開機後 Serial：udp add。
 *
 * 雲端 /api/chat 回傳 device_cmd、可選 device_target / device_link（connect|disconnect）；斷開後不送 UDP。
 *
 * --- INMP441（I2S 麥克風模組）與 ESP32-S3 建議接線（與 esp32_voice_ai_robot / esp32_i2s_audio_test 一致）---
 *   INMP441          ESP32-S3
 *   VDD  -> 3.3V   （勿接 5V）
 *   GND  -> GND
 *   WS   -> GPIO13  (LRCK)
 *   SCK  -> GPIO14  (BCLK)
 *   SD   -> GPIO12  (ESP DIN / 麥克風 DOUT)
 *   L/R  -> GND 或 3.3V（選左/右聲道）；執行時也可用 Serial 指令 micL / micR 切換（與 esp32_voice_ai_robot 一致）
 * 若同時接 MAX98357A 喇叭：DIN -> GPIO11，與麥克風共用 BCLK/WS；ENABLE_TTS=1 時會下載 /api/tts 之 MP3 並 I2S 播放（與 esp32_voice_ai_robot 相同思路）。
 * 可選：ENABLE_WIFI_AP=1 時額外開 SoftAP，手機連熱點可看 Serial 對應的 STA IP（ESP32-S3，非 ESP8266）。
 * 多個 ESP32-C3：udp list / add / use / connect / disconnect / del（見 udp help），目標存 NVS；disconnect 僅暫停送 UDP。
 * 雲端 AI：POST /api/chat 會帶 udp_targets；模型可輸出 DEVICE_TARGET（上一行）+ DEVICE_CMD，裝置依此選 IP。
 *
 * Serial 115200；ESP32-S3 若 Serial 空白可開 Tools -> USB CDC On Boot
 */

#include <Arduino.h>
#include <string.h>
#include <Preferences.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>

#ifndef ENABLE_I2S_MIC
#define ENABLE_I2S_MIC 1  // 0 = 不使用麥克風，略過 r 錄音與 ESP_I2S
#endif
#ifndef ENABLE_TTS
#define ENABLE_TTS 1  // 1 = 雲端 /api/tts 經 MAX98357A 播放 AI 文字回覆（需 I2S 與喇叭）
#endif
#if !ENABLE_I2S_MIC
#undef ENABLE_TTS
#define ENABLE_TTS 0  // 無 I2S 時無法播 MP3
#endif
#ifndef MIC_MAX_RECORD_SECONDS
// 與 esp32_voice_ai_robot 的 MAX_REC_SECONDS 一致（安全上限）；無 PSRAM 時分配會自動降秒數
#define MIC_MAX_RECORD_SECONDS 5
#endif

#ifndef ENABLE_WIFI_AP
#define ENABLE_WIFI_AP 1  // 1 = 同時開 WiFi 熱點 SoftAP（與連家裡 WiFi 的 STA 並存）
#endif
#ifndef AP_SSID
#define AP_SSID "ESP32S3-AI"
#endif
#ifndef AP_PASS
#define AP_PASS "12345678"  // WPA2 至少 8 字；可在 wifi_secrets.h 覆寫
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
#define TTS_URL   "https://magicwandmain-production.up.railway.app/api/tts"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define LOG_URL  "https://magicwandmain-production.up.railway.app/api/log"
#define DEVICE_ID "esp32s3_ai_client_001"
#define AP_SSID "ESP32S3-AI"
#define AP_PASS "12345678"
#endif

#ifndef UDP_TARGET_IP
#define UDP_TARGET_IP ""  // 可選：在 wifi_secrets.h 覆寫；空則僅能靠 Serial「udp add」
#endif
#ifndef UDP_TARGET_PORT
#define UDP_TARGET_PORT 4210
#endif

#if ENABLE_I2S_MIC && !defined(STT_URL)
#define STT_URL "https://magicwandmain-production.up.railway.app/api/stt"
#endif
#if ENABLE_I2S_MIC && ENABLE_TTS && !defined(TTS_URL)
#define TTS_URL "https://magicwandmain-production.up.railway.app/api/tts"
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

#ifndef UDP_TARGET_MAX
#define UDP_TARGET_MAX 8
#endif

static String g_udpNames[UDP_TARGET_MAX];
static IPAddress g_udpIps[UDP_TARGET_MAX];
static int g_udpCount = 0;
static int g_udpCurrent = 0;
/** false = 已斷開：不送 UDP（清單與選中仍保留；udp connect 可恢復） */
static bool g_udpSendEnabled = true;

static bool parseIp(const char* s, IPAddress& out) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static void syncTargetIpFromIndex() {
  if (g_udpCount > 0 && g_udpCurrent >= 0 && g_udpCurrent < g_udpCount) {
    g_targetIp = g_udpIps[g_udpCurrent];
  } else {
    g_targetIp = IPAddress(0, 0, 0, 0);
  }
}

static void udpPairsFromString(const String& s) {
  g_udpCount = 0;
  unsigned start = 0;
  while (start < s.length() && g_udpCount < UDP_TARGET_MAX) {
    int semi = s.indexOf(';', (int)start);
    String seg = (semi < 0) ? s.substring(start) : s.substring(start, (unsigned)semi);
    seg.trim();
    if (seg.length() == 0) break;
    int pipe = seg.indexOf('|');
    if (pipe < 0) break;
    String nm = seg.substring(0, pipe);
    String ipStr = seg.substring(pipe + 1);
    nm.trim();
    ipStr.trim();
    IPAddress ip;
    if (nm.length() && parseIp(ipStr.c_str(), ip)) {
      g_udpNames[g_udpCount] = nm;
      g_udpIps[g_udpCount] = ip;
      g_udpCount++;
    }
    if (semi < 0) break;
    start = (unsigned)semi + 1;
  }
}

static String udpPairsToString() {
  String o;
  for (int i = 0; i < g_udpCount; i++) {
    if (i) o += ";";
    o += g_udpNames[i];
    o += "|";
    o += g_udpIps[i].toString();
  }
  return o;
}

static void udpTargetsSave() {
  Preferences p;
  if (!p.begin("c3udp", false)) return;
  p.putString("pairs", udpPairsToString());
  p.putUInt("cur", (unsigned)g_udpCurrent);
  p.putBool("send", g_udpSendEnabled);
  p.end();
}

static void udpTargetsLoad() {
  Preferences p;
  if (!p.begin("c3udp", true)) {
    g_udpSendEnabled = true;
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      udpTargetsSave();
    }
    syncTargetIpFromIndex();
    return;
  }
  String pairs = p.getString("pairs", "");
  g_udpCurrent = (int)p.getUInt("cur", 0);
  g_udpSendEnabled = p.getBool("send", true);
  p.end();

  if (pairs.length() == 0) {
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      udpTargetsSave();
    }
  } else {
    udpPairsFromString(pairs);
    if (g_udpCount == 0) {
      IPAddress ip;
      if (parseIp(UDP_TARGET_IP, ip)) {
        g_udpNames[0] = "default";
        g_udpIps[0] = ip;
        g_udpCount = 1;
        g_udpCurrent = 0;
        udpTargetsSave();
      }
    } else if (g_udpCurrent < 0 || g_udpCurrent >= g_udpCount) {
      g_udpCurrent = 0;
      udpTargetsSave();
    }
  }
  syncTargetIpFromIndex();
}

static void udpPrintHelp() {
  Serial.println("--- UDP 多個 ESP32-C3 ---");
  Serial.println("udp list              列出已儲存目標與目前選中");
  Serial.println("udp add <名稱> <IP>   新增（例: udp add room1 192.168.1.50）");
  Serial.println("udp use <名稱|序號>  切換目標並連接送出（同 connect）");
  Serial.println("udp connect [名稱|序號]  連接：有參數時同 udp use；無參數時只恢復送出");
  Serial.println("udp disconnect        斷開：暫停送 UDP（清單保留，之後 udp connect）");
  Serial.println("udp del <名稱|序號>  刪除一筆");
  Serial.println("udp clear             清空 NVS；若有 UDP_TARGET_IP 則恢復一筆 default");
}

static void udpPrintList() {
  Serial.println("--- UDP 目標 (port=" + String(UDP_TARGET_PORT) + ") ---");
  for (int i = 0; i < g_udpCount; i++) {
    Serial.print(i);
    Serial.print(": ");
    Serial.print(g_udpNames[i]);
    Serial.print("  ");
    Serial.print(g_udpIps[i]);
    if (i == g_udpCurrent) Serial.print("  <-- 目前");
    Serial.println();
  }
  Serial.print("UDP 送出: ");
  Serial.println(g_udpSendEnabled ? "開啟（已連接）" : "關閉（已斷開，不送指令）");
  Serial.print("目前送出指令 -> ");
  Serial.print(g_targetIp);
  Serial.print(":");
  Serial.println(UDP_TARGET_PORT);
}

static int udpFindName(const String& name) {
  for (int i = 0; i < g_udpCount; i++) {
    if (g_udpNames[i].equalsIgnoreCase(name)) return i;
  }
  return -1;
}

/** 依名稱或序號選中目標；失敗回傳 false（不改 g_udpSendEnabled） */
static bool udpSelectByNameOrIndex(const String& u) {
  String u2 = u;
  u2.trim();
  if (u2.length() == 0) return false;
  int idx = -1;
  bool allDigits = true;
  for (unsigned i = 0; i < u2.length(); i++) {
    if (u2[i] < '0' || u2[i] > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits && u2.length() > 0) {
    idx = u2.toInt();
  } else {
    idx = udpFindName(u2);
  }
  if (idx < 0 || idx >= g_udpCount) return false;
  g_udpCurrent = idx;
  syncTargetIpFromIndex();
  return true;
}

/** 依 /api/chat 的 device_target 切換目前 UDP 目標；空或 NONE 表示不切換；名稱不存在回傳 false */
static bool udpResolveAiDeviceTarget(const String& raw) {
  String s = raw;
  s.trim();
  if (s.length() == 0) return true;
  if (s.equalsIgnoreCase("none")) return true;
  int idx = udpFindName(s);
  if (idx < 0) {
    Serial.print("[UDP] AI 目標名稱未找到: ");
    Serial.println(s);
    return false;
  }
  g_udpCurrent = idx;
  udpTargetsSave();
  syncTargetIpFromIndex();
  Serial.print("[UDP] AI 選目標 -> ");
  Serial.print(g_udpNames[g_udpCurrent]);
  Serial.print(" ");
  Serial.println(g_targetIp);
  return true;
}

/** 依 /api/chat 的 device_link：connect=允許送 UDP；disconnect=暫停送（與 Serial udp disconnect 同效果） */
static void udpApplyAiDeviceLink(const String& raw) {
  String s = raw;
  s.trim();
  if (s.length() == 0) return;
  String sl = s;
  sl.toLowerCase();
  if (sl == "none") return;
  if (sl == "disconnect") {
    g_udpSendEnabled = false;
    udpTargetsSave();
    Serial.println("[UDP] AI：斷開連接（不送 UDP，直至 AI 或 Serial 恢復連接）");
    return;
  }
  if (sl == "connect") {
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] AI：恢復連接（允許送 UDP）");
  }
}

/** Serial 行以 udp 開頭則處理並回傳 true（不送 AI） */
static bool tryHandleUdpTargetLine(const String& line) {
  if (line.length() < 3) return false;
  if (!line.substring(0, 3).equalsIgnoreCase("udp")) return false;

  String rest = line.substring(3);
  rest.trim();
  if (rest.length() == 0 || rest.equalsIgnoreCase("help")) {
    udpPrintHelp();
    return true;
  }

  String rlow = rest;
  rlow.toLowerCase();

  if (rlow == "list") {
    udpPrintList();
    return true;
  }

  if (rlow == "disconnect") {
    g_udpSendEnabled = false;
    udpTargetsSave();
    Serial.println("[UDP] 已斷開：暫不向裝置送 UDP（udp connect 可恢復）");
    udpPrintList();
    return true;
  }

  if (rlow == "connect" || rlow.startsWith("connect ")) {
    String u = (rlow == "connect") ? "" : rest.substring(8);
    u.trim();
    if (u.length() == 0) {
      if (!g_targetIp) {
        Serial.println("[UDP] 無有效目標，請先 udp add");
        return true;
      }
      g_udpSendEnabled = true;
      udpTargetsSave();
      Serial.println("[UDP] 已連接送出（使用目前選中目標）");
      udpPrintList();
      return true;
    }
    if (!udpSelectByNameOrIndex(u)) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] 已連接並切換目標");
    udpPrintList();
    return true;
  }

  if (rlow == "clear") {
    Preferences p;
    if (p.begin("c3udp", false)) {
      p.clear();
      p.end();
    }
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      g_udpSendEnabled = true;
      udpTargetsSave();
      syncTargetIpFromIndex();
      Serial.println("[UDP] 已清空並恢復預設 IP");
    } else {
      g_udpSendEnabled = true;
      udpTargetsSave();
      syncTargetIpFromIndex();
      Serial.println("[UDP] 已清空；未設定 UDP_TARGET_IP，請 udp add");
    }
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("add ")) {
    String a = rest.substring(4);
    a.trim();
    int sp = a.indexOf(' ');
    if (sp < 0) {
      Serial.println("[UDP] 用法: udp add <名稱> <IP>");
      return true;
    }
    String nm = a.substring(0, sp);
    String ipStr = a.substring(sp + 1);
    nm.trim();
    ipStr.trim();
    if (nm.length() == 0 || nm.indexOf('|') >= 0 || nm.indexOf(';') >= 0) {
      Serial.println("[UDP] 名稱不可為空或含 | ;");
      return true;
    }
    IPAddress ip;
    if (!parseIp(ipStr.c_str(), ip)) {
      Serial.println("[UDP] IP 格式錯誤");
      return true;
    }
    int ex = udpFindName(nm);
    if (ex >= 0) {
      g_udpIps[ex] = ip;
      Serial.println("[UDP] 已更新同名目標");
    } else {
      if (g_udpCount >= UDP_TARGET_MAX) {
        Serial.println("[UDP] 已滿，請先 udp del");
        return true;
      }
      g_udpNames[g_udpCount] = nm;
      g_udpIps[g_udpCount] = ip;
      g_udpCount++;
      Serial.println("[UDP] 已新增");
    }
    udpTargetsSave();
    syncTargetIpFromIndex();
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("use ")) {
    String u = rest.substring(4);
    u.trim();
    if (u.length() == 0) {
      Serial.println("[UDP] 用法: udp use <名稱|序號>");
      return true;
    }
    if (!udpSelectByNameOrIndex(u)) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] 已切換（已連接送出）");
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("del ")) {
    String u = rest.substring(4);
    u.trim();
    int idx = -1;
    bool allDigits = true;
    for (unsigned i = 0; i < u.length(); i++) {
      if (u[i] < '0' || u[i] > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits && u.length() > 0) {
      idx = u.toInt();
    } else {
      idx = udpFindName(u);
    }
    if (idx < 0 || idx >= g_udpCount) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    for (int i = idx; i < g_udpCount - 1; i++) {
      g_udpNames[i] = g_udpNames[i + 1];
      g_udpIps[i] = g_udpIps[i + 1];
    }
    g_udpCount--;
    if (g_udpCurrent >= g_udpCount) g_udpCurrent = g_udpCount - 1;
    if (g_udpCurrent < 0) g_udpCurrent = 0;
    udpTargetsSave();
    syncTargetIpFromIndex();
    Serial.println("[UDP] 已刪除");
    udpPrintList();
    return true;
  }

  udpPrintHelp();
  return true;
}

static bool udpSendCommand(const char* cmd) {
  if (!g_udpSendEnabled) {
    Serial.println("[UDP] 送出已關閉 — 請 udp connect 以連接並恢復");
    return false;
  }
  if (!g_targetIp) {
    Serial.println("[UDP] 無有效目標 — 執行 udp list / udp add / udp use");
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

static String udpTargetsJsonArray() {
  if (g_udpCount <= 0) return "[]";
  String j = "[";
  for (int i = 0; i < g_udpCount; i++) {
    if (i) j += ",";
    j += "{\"name\":\"";
    j += escapeJsonString(g_udpNames[i]);
    j += "\",\"ip\":\"";
    j += escapeJsonString(g_udpIps[i].toString());
    j += "\"}";
  }
  j += "]";
  return j;
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

static bool httpPostChat(const String& text, String& outReply, String& outDeviceCmd, String& outDeviceTarget,
                         String& outDeviceLink, String& outTtsUrl) {
  outReply = "";
  outDeviceCmd = "";
  outDeviceTarget = "";
  outDeviceLink = "";
  outTtsUrl = "";
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
  body += "\",\"udp_targets\":";
  body += udpTargetsJsonArray();
#if ENABLE_I2S_MIC && ENABLE_TTS
  body += ",\"include_tts\":true";
#endif
  body += "}";
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
  outDeviceTarget = extractJsonStringField(resp, "device_target");
  outDeviceLink = extractJsonStringField(resp, "device_link");
  outTtsUrl = extractJsonStringField(resp, "tts_absolute_url");
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

#if ENABLE_TTS
static String stripForTts(const String& s) {
  String t = s;
  t.replace('\r', ' ');
  t.replace('\n', ' ');
  t.trim();
  if (t.length() > 400) t = t.substring(0, 400);
  return t;
}

static bool httpPostTts(const String& text, String& outAbsoluteUrl) {
  outAbsoluteUrl = "";
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  if (!http.begin(client, TTS_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(180000);
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
  return outAbsoluteUrl.length() > 0;
}

static uint8_t* downloadMp3ToRam(const char* url, size_t& outSize) {
  outSize = 0;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  if (!http.begin(client, url)) return nullptr;
  int code = http.GET();
  if (code != 200) {
    Serial.print("[TTS dl HTTP ");
    Serial.print(code);
    Serial.println("]");
    http.end();
    return nullptr;
  }
  int len = http.getSize();
  const size_t kMax = 512 * 1024;
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (len > 0) ? (size_t)len : (64 * 1024);
  if (cap > kMax) cap = kMax;
  uint8_t* buf = nullptr;
  if (ESP.getPsramSize() > 0) {
    buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buf) buf = (uint8_t*)malloc(cap);
  if (!buf) {
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
          free(buf);
          http.end();
          return nullptr;
        }
        uint8_t* nb = (uint8_t*)realloc(buf, newCap);
        if (!nb) {
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

static bool i2sBeginForPlayback() {
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, -1);
  return I2S.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

static void speakAiReply(const String& replyText, const String& inlineTtsUrl) {
  String t = stripForTts(replyText);
  if (!t.length()) return;

  Serial.println("[TTS] 播放 AI 回覆…");
  I2S.end();
  delay(50);

  String mp3Url;
  if (inlineTtsUrl.length() > 0) {
    mp3Url = inlineTtsUrl;
    Serial.println("[TTS] 使用 /api/chat 內嵌網址（省一次 POST）");
  } else if (!httpPostTts(t, mp3Url)) {
    Serial.println("[TTS] 請求音訊 URL 失敗");
    i2sBeginForMic();
    return;
  }

  size_t mp3Len = 0;
  uint8_t* mp3 = downloadMp3ToRam(mp3Url.c_str(), mp3Len);
  if (!mp3 || mp3Len < 16) {
    Serial.println("[TTS] MP3 下載失敗");
    if (mp3) free(mp3);
    i2sBeginForMic();
    return;
  }

  if (!i2sBeginForPlayback()) {
    Serial.println("[TTS] I2S 播放模式啟動失敗");
    free(mp3);
    i2sBeginForMic();
    return;
  }

  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("[TTS] playMP3=");
  Serial.println(ok ? "ok" : "fail");
  free(mp3);
  i2sBeginForMic();
}
#endif  // ENABLE_TTS
#endif  // ENABLE_I2S_MIC

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== ESP32-S3 AI + UDP -> ESP32-C3 LED ===");
  Serial.println("Mode: 僅依 /api/chat 的 device_cmd / device_target 送 UDP（無本地語意判燈）");

#if ENABLE_WIFI_AP
  WiFi.mode(WIFI_AP_STA);
#else
  WiFi.mode(WIFI_STA);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi(STA)");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi STA IP: ");
  Serial.println(WiFi.localIP());

#if ENABLE_WIFI_AP
  if (WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.print("SoftAP SSID: ");
    Serial.print(AP_SSID);
    Serial.print("  IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("[WiFi] SoftAP 啟動失敗（檢查 AP_PASS 長度>=8）");
  }
#endif

  udpTargetsLoad();
  g_udp.begin(0);
  if (!g_targetIp) {
    Serial.println("[UDP] 無有效 C3 目標 — 請在 wifi_secrets.h 設 UDP_TARGET_IP，或 Serial: udp add …");
  } else {
    Serial.print("[UDP] 目前指令目標 ");
    Serial.print(g_targetIp);
    Serial.print(":");
    Serial.println(UDP_TARGET_PORT);
  }
  udpPrintList();
  Serial.println("多個 C3：udp help（僅設定目標；開關燈由雲端 AI 判斷）");

  Serial.println();
  Serial.println("輸入一行文字；輸入 r 開始錄音，再輸入 r 停止並上傳語音：");
#if ENABLE_I2S_MIC
  Serial.println("麥克風調校（同 esp32_voice_ai_robot）: micL / micR 聲道, shift8..shift20 增益");
#if ENABLE_TTS
  Serial.println("TTS 喇叭: ON（POST /api/tts -> MAX98357A）");
#else
  Serial.println("TTS 喇叭: OFF（ENABLE_TTS=0）");
#endif
#endif
#if ENABLE_WIFI_AP
  Serial.println("WiFi 熱點(SoftAP)已開，手機可連 SSID 見上");
#endif
}

static void processUserTextLine(const String& line) {
  String reply;
  String deviceCmd;
  String deviceTarget;
  String deviceLink;
  String ttsUrl;
  Serial.println("[CHAT] ...");
  if (!httpPostChat(line, reply, deviceCmd, deviceTarget, deviceLink, ttsUrl)) {
    Serial.println("請繼續輸入：");
    return;
  }
  Serial.print("[AI] ");
  Serial.println(reply);
  deviceCmd.toLowerCase();
  deviceCmd.trim();
  deviceTarget.trim();
  deviceLink.trim();
  Serial.print("[CLOUD] device_cmd=");
  Serial.print(deviceCmd.length() ? deviceCmd : "(empty)");
  Serial.print("  device_target=");
  Serial.print(deviceTarget.length() ? deviceTarget : "(unchanged)");
  Serial.print("  device_link=");
  Serial.println(deviceLink.length() ? deviceLink : "(unchanged)");

  httpPostLogBestEffort(line, reply + " |cmd:" + deviceCmd + "|tgt:" + deviceTarget + "|link:" + deviceLink,
                        "chat");

  udpApplyAiDeviceLink(deviceLink);

  bool targetOk = udpResolveAiDeviceTarget(deviceTarget);
  if (!targetOk) {
    Serial.println("[PATH] AI device_target 無效，不送出 UDP");
  }

  if (targetOk && deviceCmd == "on") {
    Serial.println("[PATH] AI device_cmd -> UDP ON");
    udpSendCommand("ON");
    httpPostLogBestEffort(line, "ON", "ai_device_cmd");
  } else if (targetOk && deviceCmd == "off") {
    Serial.println("[PATH] AI device_cmd -> UDP OFF");
    udpSendCommand("OFF");
    httpPostLogBestEffort(line, "OFF", "ai_device_cmd");
  } else if (targetOk && (deviceCmd == "b0" || deviceCmd == "b25" || deviceCmd == "b50" || deviceCmd == "b75" ||
                          deviceCmd == "b100")) {
    String u = deviceCmd;
    u.toUpperCase();
    Serial.print("[PATH] AI device_cmd -> UDP ");
    Serial.println(u);
    udpSendCommand(u.c_str());
    httpPostLogBestEffort(line, u, "ai_device_cmd");
  }

#if ENABLE_I2S_MIC && ENABLE_TTS
  speakAiReply(reply, ttsUrl);
#endif

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

  if (tryHandleUdpTargetLine(line)) return;

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
