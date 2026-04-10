// UDP receiver for ESP8266 or ESP32 (including ESP32-C3).
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <WiFiUdp.h>
#include <string.h>

// ---------- WiFi ----------
// Put secrets in wifi_secrets.h if you want.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#endif

// ---------- UDP ----------
static const uint16_t UDP_PORT = 4210;
WiFiUDP udp;

// Output pin (use raw GPIO numbers for compatibility).
// - ESP8266 NodeMCU/Wemos D1 mini: GPIO5 is often labeled "D1"
// - ESP32-C3 dev boards: GPIO2 is a common safe choice (verify your board)
#if defined(ESP8266)
static const int RELAY_PIN = 5;   // GPIO5 (often "D1")
#else
static const int RELAY_PIN = 2;   // GPIO2 (typical)
#endif

static void reply(const char* msg) {
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.print(msg);
  udp.endPacket();
}

// Trim spaces and CR/LF so strcmp("ON") works if sender adds \r\n
static void trimInPlace(char* s) {
  if (!s) return;
  // leading
  char* p = s;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  // trailing
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
    s[--len] = '\0';
  }
}

static bool cmdEqCi(const char* s, const char* ref) {
  for (; *ref; s++, ref++) {
    if (!*s) return false;
    char a = *s, b = *ref;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return *s == '\0';
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("========== [BOOT] UDP Server starting ==========");
  Serial.println("[BOOT] Sketch: esp8266_udp_server.ino");
  Serial.print("[BOOT] Chip: ");
#if defined(ESP8266)
  Serial.println("ESP8266");
#else
  Serial.println("ESP32 (incl. ESP32-C3 when built with esp32 core)");
#endif
  Serial.print("[BOOT] RELAY_PIN GPIO = ");
  Serial.println(RELAY_PIN);
  Serial.println("[BOOT] Valid UDP payload: ON / OFF (case-insensitive, trim spaces/CR/LF)");
  Serial.println("[BOOT] On success you will see [RESULT] OK ...");
  Serial.println("[BOOT] On bad payload you will see [RESULT] FAIL ...");
  Serial.println("================================================");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[BOOT] WiFi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println();
      Serial.print("[ERR] WiFi connect timeout. status=");
      Serial.println((int)WiFi.status());
      Serial.println("Check WIFI_SSID/WIFI_PASS (wifi_secrets.h) and use 2.4GHz WiFi.");
      break;
    }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[BOOT] WiFi OK. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[BOOT] WiFi not connected — UDP may not receive from LAN");
  }

  udp.begin(UDP_PORT);
  Serial.print("[BOOT] UDP listening on port ");
  Serial.println(UDP_PORT);
  Serial.println("========== [BOOT] READY — waiting for UDP ==========");
}

void loop() {
  int packetSize = udp.parsePacket();
  if (!packetSize) return;

  char buf[128];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  trimInPlace(buf);

  Serial.print("[RX] UDP from ");
  Serial.print(udp.remoteIP());
  Serial.print(":");
  Serial.print(udp.remotePort());
  Serial.print(" raw_bytes=");
  Serial.print(n);
  Serial.print(" trimmed=\"");
  Serial.print(buf);
  Serial.println("\"");

  if (cmdEqCi(buf, "ON")) {
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("[OUT] RELAY_PIN=HIGH");
    Serial.println("[RESULT] OK — command recognized: ON (valid)");
    reply("OK ON");
    return;
  }

  if (cmdEqCi(buf, "OFF")) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("[OUT] RELAY_PIN=LOW");
    Serial.println("[RESULT] OK — command recognized: OFF (valid)");
    reply("OK OFF");
    return;
  }

  Serial.println("[RESULT] FAIL — payload is not ON/OFF (invalid command)");
  Serial.print("[DBG] len=");
  Serial.print(strlen(buf));
  Serial.print(" hex:");
  for (size_t i = 0; i < strlen(buf) && i < 16; i++) {
    Serial.print(" ");
    if ((uint8_t)buf[i] < 16) Serial.print("0");
    Serial.print((uint8_t)buf[i], HEX);
  }
  Serial.println();
  reply("ERR unknown");
}

