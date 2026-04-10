/*
 * ESP32 Serial AI Robot（文字版，無麥克風 / 無 STT）
 *
 * 與 esp32_voice_ai_robot 相同後端與 UDP 邏輯，但改為：
 *   Serial Monitor 輸入一行文字 → 若符合簡易指令則 UDP 送 ESP8266，否則 POST /api/chat
 *
 * Serial 115200，每行一則訊息（NL 或 CRLF 皆可）。
 */

#include <Arduino.h>
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
#define DEVICE_ID "esp32_serial_robot_001"
#define ESP8266_IP "10.232.188.113"
#define ESP8266_UDP_PORT 4210
#endif

static WiFiUDP g_udp;
static IPAddress g_esp8266Ip;

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

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== ESP32 Serial AI Robot (text input) ===");
  Serial.println("連線後請輸入一行文字並按 Enter；開/關燈關鍵字會經 UDP 送 ESP8266。");

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
    g_udp.begin(0);
    Serial.print("[UDP] target ESP8266=");
    Serial.print(g_esp8266Ip);
    Serial.print(":");
    Serial.println((int)ESP8266_UDP_PORT);
  }

  Serial.println();
  Serial.println("請輸入：");
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
    Serial.println("[錯誤] 單次請勿超過 2000 字元");
    return;
  }

  Serial.print("[YOU] ");
  Serial.println(line);

  String cmdUdp, say;
  if (matchVoiceCommand(line, cmdUdp, say)) {
    Serial.print("[CMD] ");
    Serial.println(cmdUdp);
    udpSendToEsp8266(cmdUdp.c_str());
    httpPostLogBestEffort(line, cmdUdp, "command");
    if (say.length()) {
      Serial.print("[提示] ");
      Serial.println(say);
    }
    Serial.println("[DONE]");
    Serial.println("請繼續輸入：");
    return;
  }

  String reply;
  Serial.println("[CHAT] Asking...");
  if (!httpPostChat(line, reply)) {
    Serial.println("請繼續輸入：");
    return;
  }
  Serial.print("[AI] ");
  Serial.println(reply);
  httpPostLogBestEffort(line, reply, "chat");
  Serial.println("[DONE]");
  Serial.println("請繼續輸入：");
}
