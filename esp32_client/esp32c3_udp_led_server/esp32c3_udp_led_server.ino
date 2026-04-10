/*
 * ESP32-C3 — WiFi + UDP LED 接收端
 *
 * - 連 WiFi (STA)
 * - UDP 監聽埠 4210，收到 "ON" / "OFF" 控制外接 LED（預設 GPIO4，高電平亮）
 * - 供 ESP32-S3「AI 客戶端」以 UDP 送指令
 *
 * 硬體：GPIO4 -> 220~470Ω -> LED 長腳 -> LED 短腳 -> GND
 * Arduino：開發板選 ESP32C3 Dev Module；USB CDC On Boot 建議 Enabled（Serial 除錯）
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#endif

static const uint16_t UDP_PORT = 4210;
WiFiUDP udp;

// 與 esp8266_led_test 外接 LED 一致：GPIO4，active-HIGH
static const int LED_PIN = 4;

// PWM (LEDC) for brightness control (0/25/50/75/100%)
static const uint32_t PWM_FREQ_HZ = 5000;
static const uint8_t PWM_RES_BITS = 8;  // duty 0..255

// NOTE: This sketch uses the Arduino-ESP32 v3.x LEDC API:
//   ledcAttach(pin, freq, resolution) + ledcWrite(pin, duty)
// If you are on an older Arduino-ESP32 core (2.x), upgrade the ESP32 board package.
static bool pwmBegin() {
  return ledcAttach(LED_PIN, PWM_FREQ_HZ, PWM_RES_BITS);
}

static void pwmWriteDuty(uint32_t duty) {
  ledcWrite(LED_PIN, duty);
}

static void setBrightnessPercent(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint32_t dutyMax = (1u << PWM_RES_BITS) - 1u;
  uint32_t duty = (uint32_t)((pct * (int)dutyMax + 50) / 100);  // rounded
  pwmWriteDuty(duty);
  Serial.print("[PWM] brightness=");
  Serial.print(pct);
  Serial.print("% duty=");
  Serial.println((int)duty);
}

static void replyUdp(const char* msg) {
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.print(msg);
  udp.endPacket();
}

static void trimInPlace(char* s) {
  if (!s) return;
  char* p = s;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
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
  delay(800);  // USB CDC: give host time to attach

  Serial.println();
  Serial.println("========== [BOOT] ESP32-C3 UDP LED Server ==========");
  Serial.println("[BOOT] Sketch: esp32c3_udp_led_server.ino");
  Serial.print("[BOOT] LED GPIO = ");
  Serial.print(LED_PIN);
  Serial.println(" (external LED, PWM capable)");

  // PWM setup
  if (!pwmBegin()) {
    Serial.println("[ERR] PWM init failed (LEDC attach)");
  }
  setBrightnessPercent(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[BOOT] WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - t0 > 25000) {
      Serial.println();
      Serial.println("[ERR] WiFi timeout — check SSID/PASS, use 2.4GHz");
      break;
    }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[BOOT] WiFi OK  IP: ");
    Serial.println(WiFi.localIP());
  }

  udp.begin(UDP_PORT);
  Serial.print("[BOOT] UDP listen port ");
  Serial.println(UDP_PORT);
  Serial.println("[BOOT] Payload: ON / OFF / B0 / B25 / B50 / B75 / B100");
  Serial.println("========== [BOOT] READY =================================");
}

void loop() {
  if (!udp.parsePacket()) return;

  char buf[128];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  trimInPlace(buf);

  Serial.print("[RX] from ");
  Serial.print(udp.remoteIP());
  Serial.print(":");
  Serial.print(udp.remotePort());
  Serial.print(" \"");
  Serial.print(buf);
  Serial.println("\"");

  if (cmdEqCi(buf, "ON")) {
    setBrightnessPercent(100);
    Serial.println("[RESULT] OK — ON -> brightness 100%");
    replyUdp("OK ON 100");
    return;
  }
  if (cmdEqCi(buf, "OFF")) {
    setBrightnessPercent(0);
    Serial.println("[RESULT] OK — OFF -> brightness 0%");
    replyUdp("OK OFF 0");
    return;
  }

  // Brightness commands: B0/B25/B50/B75/B100 (case-insensitive)
  if (buf[0] == 'B' || buf[0] == 'b') {
    int pct = atoi(buf + 1);
    if (pct == 0 || pct == 25 || pct == 50 || pct == 75 || pct == 100) {
      setBrightnessPercent(pct);
      Serial.println("[RESULT] OK — brightness set");
      char resp[32];
      snprintf(resp, sizeof(resp), "OK B%d", pct);
      replyUdp(resp);
      return;
    }
    Serial.println("[RESULT] FAIL — brightness must be 0/25/50/75/100");
    replyUdp("ERR bad brightness");
    return;
  }

  Serial.println("[RESULT] FAIL — need ON/OFF/Bxx");
  replyUdp("ERR unknown");
}
