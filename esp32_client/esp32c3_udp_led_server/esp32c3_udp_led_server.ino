/*
 * ESP32-C3 — WiFi + UDP + BLE(Nordic UART) PWM 馬達/風扇接收端
 *
 * - 自動連 WiFi (STA)，斷線會重試
 * - UDP 監聽埠 4210，收到 "ON" / "OFF" / "Bxx" 輸出 PWM 到馬達模塊 IN
 * - BLE：廣播名稱 MagicWand-C3，Nordic UART RX 寫入與 UDP 相同字串
 *
 * 指令：
 * - ON  -> 100%
 * - OFF -> 0%
 * - B0/B25/B50/B75/B100 -> 對應占空比
 *
 * 接線（常見三線馬達模塊：GND/VCC/IN）：
 * - 模塊 GND -> ESP32-C3 GND
 * - 模塊 VCC -> 5V（VBUS/5V 腳或外部 5V），需與 ESP32-C3 共地
 * - 模塊 IN  -> ESP32-C3 的 PWM 腳（預設 GPIO4，可改 MOTOR_PIN）
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

#ifndef ENABLE_BLE_SERVER
#define ENABLE_BLE_SERVER 1
#endif
#if ENABLE_BLE_SERVER
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#endif

static const uint16_t UDP_PORT = 4210;
WiFiUDP udp;

static const int MOTOR_PIN = 4;
static const uint32_t PWM_FREQ_HZ = 5000;
static const uint8_t PWM_RES_BITS = 8;

// Nordic UART Service（與 ESP32-S3 客戶端一致）
static const char* BLE_DEVICE_NAME = "MagicWand-C3";
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

static bool g_udpStarted = false;
static uint32_t g_wifiRetryAtMs = 0;

static void wifiUdpTick() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_udpStarted) {
      udp.begin(UDP_PORT);
      g_udpStarted = true;
      Serial.print("[BOOT] WiFi IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("[BOOT] UDP port ");
      Serial.println(UDP_PORT);
    }
    return;
  }

  if (g_udpStarted) {
    udp.stop();
    g_udpStarted = false;
  }

  uint32_t now = millis();
  if (now < g_wifiRetryAtMs) return;
  g_wifiRetryAtMs = now + 5000;

  Serial.println("[WiFi] reconnect...");
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static bool pwmBegin() {
  return ledcAttach(MOTOR_PIN, PWM_FREQ_HZ, PWM_RES_BITS);
}

static void pwmWriteDuty(uint32_t duty) {
  ledcWrite(MOTOR_PIN, duty);
}

static void setSpeedPercent(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint32_t dutyMax = (1u << PWM_RES_BITS) - 1u;
  uint32_t duty = (uint32_t)((pct * (int)dutyMax + 50) / 100);
  // 部分三線風扇/馬達模塊 IN 腳為反相（active-low）：0=全速，255=停
  // 反相後語意回到：0=停，255=全速
  duty = dutyMax - duty;
  pwmWriteDuty(duty);
  Serial.print("[PWM] speed=");
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

static void applyMotorPayload(char* buf, bool doUdpReply) {
  trimInPlace(buf);

  if (cmdEqCi(buf, "ON")) {
    setSpeedPercent(100);
    Serial.println("[RESULT] OK — ON -> 100%");
    if (doUdpReply) replyUdp("OK ON 100");
    return;
  }
  if (cmdEqCi(buf, "OFF")) {
    setSpeedPercent(0);
    Serial.println("[RESULT] OK — OFF -> 0%");
    if (doUdpReply) replyUdp("OK OFF 0");
    return;
  }
  if (buf[0] == 'B' || buf[0] == 'b') {
    int pct = atoi(buf + 1);
    if (pct == 0 || pct == 25 || pct == 50 || pct == 75 || pct == 100) {
      setSpeedPercent(pct);
      Serial.println("[RESULT] OK — speed set");
      char resp[32];
      snprintf(resp, sizeof(resp), "OK B%d", pct);
      if (doUdpReply) replyUdp(resp);
      return;
    }
    Serial.println("[RESULT] FAIL — speed must be 0/25/50/75/100");
    if (doUdpReply) replyUdp("ERR bad speed");
    return;
  }
  Serial.println("[RESULT] FAIL — need ON/OFF/Bxx");
  if (doUdpReply) replyUdp("ERR unknown");
}

#if ENABLE_BLE_SERVER
class NusRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    // Arduino-ESP32 3.x：getValue() 回傳 String，非 std::string
    String v = pCharacteristic->getValue();
    if (v.length() == 0 || v.length() >= 127) return;
    char buf[128];
    v.toCharArray(buf, sizeof(buf));
    Serial.print("[RX-BLE] \"");
    Serial.print(buf);
    Serial.println("\"");
    applyMotorPayload(buf, false);
  }
};

static void bleServerStart() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  BLEService* svc = pServer->createService(NUS_SERVICE_UUID);
  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new NusRxCallbacks());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.print("[BOOT] BLE advertising as \"");
  Serial.print(BLE_DEVICE_NAME);
  Serial.println("\" (Nordic UART RX, Bluedroid)");
}
#endif

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("========== [BOOT] ESP32-C3 UDP+BLE MOTOR Server ==========");

  if (!pwmBegin()) {
    Serial.println("[ERR] PWM init failed");
  }
  setSpeedPercent(0);

#if ENABLE_BLE_SERVER
  bleServerStart();
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[BOOT] WiFi auto-connect enabled (will retry)");

  Serial.println("[BOOT] Payload: ON / OFF / B0 / B25 / B50 / B75 / B100");
  Serial.println("========== [BOOT] READY =================================");
}

void loop() {
  wifiUdpTick();

  if (WiFi.status() != WL_CONNECTED || !g_udpStarted) {
    delay(10);
    return;
  }
  if (!udp.parsePacket()) {
    delay(1);
    return;
  }

  char buf[128];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  trimInPlace(buf);

  Serial.print("[RX-UDP] from ");
  Serial.print(udp.remoteIP());
  Serial.print(":");
  Serial.print(udp.remotePort());
  Serial.print(" \"");
  Serial.print(buf);
  Serial.println("\"");
  applyMotorPayload(buf, true);
}
