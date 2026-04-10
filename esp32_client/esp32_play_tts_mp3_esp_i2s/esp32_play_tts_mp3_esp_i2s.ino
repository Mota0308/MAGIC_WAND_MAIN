/*
 * ESP32-S3: Download MP3 (Poe TTS proxy) then play via ESP_I2S (no ESP32-audioI2S).
 *
 * Why:
 * - Avoids long URL / redirect / streaming issues in ESP32-audioI2S.
 * - Your TTS MP3 is small (~24KB), so we can download into RAM and play.
 *
 * Requirements:
 * - Arduino-ESP32 3.x (provides <ESP_I2S.h>)
 *
 * Wiring (MAX98357A):
 * - VIN -> 5V, GND -> GND
 * - BCLK -> GPIO14
 * - LRC  -> GPIO13
 * - DIN  -> GPIO11
 * - Speaker -> pads marked + / -
 * - If SD pin exists: SD -> 3.3V
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
// Use the short proxy URL returned by POST /api/tts (absolute_url).
#define TTS_MP3_URL "https://magicwandmain-production.up.railway.app/api/tts/audio/erwdjhziKqlUSg"
#endif

// I2S pins (your setup)
static const int PIN_BCLK = 14;
static const int PIN_WS   = 13;
static const int PIN_DOUT = 11;

I2SClass I2S;

static uint8_t* downloadToRam(const char* url, size_t& outSize) {
  outSize = 0;
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

  int len = http.getSize(); // -1 if unknown
  // We expect small files; cap to prevent OOM if something goes wrong.
  const size_t kMax = 512 * 1024;

  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (len > 0) ? (size_t)len : (64 * 1024);
  if (cap > kMax) cap = kMax;

  uint8_t* buf = (uint8_t*)malloc(cap);
  if (!buf) {
    Serial.println("[ERR] malloc failed");
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
        // grow if length unknown and we need more
        size_t newCap = cap * 2;
        if (newCap < pos + toRead) newCap = pos + toRead;
        if (newCap > kMax) {
          Serial.println("[ERR] file too large");
          free(buf);
          http.end();
          return nullptr;
        }
        uint8_t* nb = (uint8_t*)realloc(buf, newCap);
        if (!nb) {
          Serial.println("[ERR] realloc failed");
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
      // timeout protection
      if (millis() - start > 20000) break;
      delay(5);
    }
    if (len > 0 && (int)pos >= len) break;
  }

  http.end();
  outSize = pos;
  return buf;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32 Play TTS MP3 via ESP_I2S ===");

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

  Serial.println("Downloading MP3...");
  Serial.println(TTS_MP3_URL);
  size_t mp3Len = 0;
  uint8_t* mp3 = downloadToRam(TTS_MP3_URL, mp3Len);
  if (!mp3 || mp3Len < 16) {
    Serial.println("[ERR] download failed");
    while (true) delay(1000);
  }
  Serial.print("[OK] downloaded bytes=");
  Serial.println(mp3Len);

  Serial.println("Playing MP3...");
  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("playMP3 ok=");
  Serial.println(ok ? "true" : "false");

  free(mp3);
  Serial.println("Done.");
}

void loop() {
  delay(1000);
}

