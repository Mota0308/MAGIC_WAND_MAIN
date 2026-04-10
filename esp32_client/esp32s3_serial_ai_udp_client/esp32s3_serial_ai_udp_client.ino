/*
 * ESP32-S3 — Serial 輸入 -> AI (/api/chat) + 可選 UDP 控制 ESP32-C3 LED
 *
 * 流程：
 *   1) Serial 讀一行
 *   2) 若符合開/關燈關鍵字 -> UDP 送 ON/OFF 到 ESP32-C3（不經雲端）
 *   3) 否則 POST /api/chat；若 AI 回覆裡仍含開/關語意，再送 UDP
 *
 * C3 端請燒錄：esp32c3_udp_led_server.ino，並把本機 UDP_TARGET_IP 設為 C3 的 IP。
 *
 * 雲端 /api/chat 會回傳 JSON 欄位 device_cmd: ON | OFF | NONE | B0 | B25 | B50 | B75 | B100（由 AI 依 system prompt 判斷亮度檔位）。
 * 設 SERIAL_AI_FORCE_CLOUD 為 1 可略過本地關鍵字、一律走雲端 AI。
 *
 * Serial 115200；ESP32-S3 若 Serial 空白可開 Tools -> USB CDC On Boot
 */

#include <Arduino.h>

#ifndef SERIAL_AI_FORCE_CLOUD
#define SERIAL_AI_FORCE_CLOUD 1  // 1 = 全部交給雲端 AI 判斷 device_cmd
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define LOG_URL  "https://magicwandmain-production.up.railway.app/api/log"
#define DEVICE_ID "esp32s3_ai_client_001"
#define UDP_TARGET_IP   "10.232.188.113"
#define UDP_TARGET_PORT 4210
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
  Serial.println("輸入一行文字：");
}

void loop() {
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

  Serial.print("[YOU] ");
  Serial.println(line);

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
