#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ---------- WiFi ----------
// Put secrets in wifi_secrets.h if you want.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
// Replace with the ESP8266 IP printed in its Serial Monitor.
#define ESP8266_IP "10.255.142.157"
#endif

// ---------- UDP ----------
static const uint16_t UDP_PORT = 4210;
WiFiUDP udp;
IPAddress esp8266Ip;

static bool parseIp(const char* s, IPAddress& out) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static void sendCmd(const char* cmd) {
  udp.beginPacket(esp8266Ip, UDP_PORT);
  udp.print(cmd);
  udp.endPacket();
  Serial.print("Sent: ");
  Serial.println(cmd);

  // Optional: wait up to 500ms for reply
  unsigned long start = millis();
  while (millis() - start < 500) {
    int n = udp.parsePacket();
    if (n) {
      char buf[128];
      int r = udp.read(buf, sizeof(buf) - 1);
      if (r < 0) r = 0;
      buf[r] = '\0';
      Serial.print("Reply: ");
      Serial.println(buf);
      return;
    }
    delay(10);
  }
  Serial.println("No reply");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== ESP32 UDP Client ===");

  if (!parseIp(ESP8266_IP, esp8266Ip)) {
    Serial.println("[ERR] ESP8266_IP is invalid. Set it to x.x.x.x");
    while (true) delay(1000);
  }

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

  udp.begin(0); // any local port
  Serial.print("Target ESP8266: ");
  Serial.print(esp8266Ip);
  Serial.print(":");
  Serial.println(UDP_PORT);
  Serial.println("Type ON or OFF then Enter");
}

void loop() {
  if (!Serial.available()) return;
  String s = Serial.readStringUntil('\n');
  s.trim();
  s.toUpperCase();
  if (s == "ON" || s == "OFF") {
    sendCmd(s.c_str());
  } else if (s.length()) {
    Serial.println("Commands: ON / OFF");
  }
}

