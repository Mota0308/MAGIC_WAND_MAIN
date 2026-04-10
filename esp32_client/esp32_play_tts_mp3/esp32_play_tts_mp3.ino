/*
 * ESP32-S3: Play Poe TTS MP3 via Railway proxy URL (MAX98357A)
 *
 * Requirements:
 * - Library: "ESP32-audioI2S" by schreibfaul1 (provides <Audio.h>)
 *
 * Wiring (your setup):
 * - MAX98357A VIN -> 5V, GND -> GND
 * - MAX98357A BCLK -> GPIO14
 * - MAX98357A LRC  -> GPIO13
 * - MAX98357A DIN  -> GPIO11
 * - Speaker -> module pads marked + / -
 *
 * Notes:
 * - Use the backend proxy URL (NOT the poecdn.net upstream) to avoid AccessDenied:
 *   https://magicwandmain-production.up.railway.app/api/tts/audio?u=...
 * - If your amp has SD pin, tie SD to 3.3V to enable.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Audio.h>  // schreibfaul1/ESP32-audioI2S

// ---------- Config (prefer wifi_secrets.h) ----------
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
// Fill these before upload if you don't use wifi_secrets.h
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
// Paste FULL proxy URL returned by POST /api/tts (url field), prefixed with domain.
#define TTS_MP3_URL "https://magicwandmain-production.up.railway.app/api/tts/audio/xO5MtG6vbOFmTg"
#endif
// ---------------------------------------------------

// I2S pinout for MAX98357A
static const int I2S_BCLK = 14;
static const int I2S_LRC  = 13;
static const int I2S_DOUT = 11;

Audio audio;

static void audioInfo(Audio::msg_t m) {
  Serial.printf("%s: %s\n", m.s, m.msg);
}

static void printAudioStats() {
  Serial.print("running=");
  Serial.print(audio.isRunning() ? "1" : "0");
  Serial.print(" codec=");
  Serial.print(audio.getCodecname());
  Serial.print(" sr=");
  Serial.print(audio.getSampleRate());
  Serial.print("Hz bits=");
  Serial.print(audio.getBitsPerSample());
  Serial.print(" ch=");
  Serial.print(audio.getChannels());
  Serial.print(" br=");
  Serial.print(audio.getBitRate());
  Serial.print(" fileSize=");
  Serial.print(audio.getFileSize());
  Serial.print(" pos=");
  Serial.print(audio.getAudioFilePosition());
  Serial.print(" t=");
  Serial.print(audio.getAudioCurrentTime());
  Serial.println("s");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32 Play TTS MP3 (ESP32-audioI2S) ===");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  Audio::audio_info_callback = audioInfo;

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

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(18);  // 0..21

  Serial.println("Connecting to TTS URL...");
  Serial.println(TTS_MP3_URL);
  audio.setConnectionTimeout(10000, 10000);
  audio.connecttohost(TTS_MP3_URL);
}

void loop() {
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 1000) {
    lastBeat = millis();
    Serial.print("loop alive, free heap=");
    Serial.println(ESP.getFreeHeap());
    printAudioStats();
  }
  audio.loop();
  delay(1);
}

