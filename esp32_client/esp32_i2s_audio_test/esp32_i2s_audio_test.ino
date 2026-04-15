/*
 * ESP32-S3 I2S audio hardware test (Arduino-ESP32 3.x / ESP_I2S.h)
 *
 * 預設與 esp32s3_serial_ai_udp_client.ino 相同：麥、喇叭分線（不並接 BCLK/WS）。
 * 若你的板子是「麥與喇叭共用 14/13 時鐘」，把 I2S_TEST_SHARED_BUS 改為 1。
 *
 * 分線預設：
 *   麥 INMP441  SCK->14  WS->13  SD->12  3.3V/GND/L/R
 *   喇叭 MAX98357  BCLK->17  LRC->18  DIN->11  VIN/GND/Speaker
 *
 * 共用總線（I2S_TEST_SHARED_BUS=1）：
 *   BCLK=14  WS=13  ESP DOUT->功放 DIN=11  INMP441 SD->ESP DIN=12
 */

#include <Arduino.h>
#include <ESP_I2S.h>

// ---------- 依實際焊接修改；與主程式 wifi_secrets / 宏一致最省事 ----------
// 1 = 麥與喇叭共用同一組 BCLK/WS（只需 14/13/11/12 四條信號到 ESP）
// 0 = 分線：麥用 PIN_MIC_*，喇叭用 PIN_SPK_*
#ifndef I2S_TEST_SHARED_BUS
#define I2S_TEST_SHARED_BUS 0
#endif

#if I2S_TEST_SHARED_BUS
static const int PIN_I2S_BCLK = 14;
static const int PIN_I2S_WS = 13;
static const int PIN_I2S_DOUT = 11;
static const int PIN_I2S_DIN = 12;
#else
#ifndef PIN_MIC_BCLK
#define PIN_MIC_BCLK 14
#endif
#ifndef PIN_MIC_WS
#define PIN_MIC_WS 13
#endif
#ifndef PIN_MIC_DIN
#define PIN_MIC_DIN 12
#endif
#ifndef PIN_MIC_DOUT_UNUSED
#define PIN_MIC_DOUT_UNUSED (-1)
#endif
#ifndef PIN_SPK_BCLK
#define PIN_SPK_BCLK 17
#endif
#ifndef PIN_SPK_WS
#define PIN_SPK_WS 18
#endif
#ifndef PIN_SPK_DOUT
#define PIN_SPK_DOUT 11
#endif
#endif

// INMP441 常只一邊有聲：true=用右聲道 raw，false=左
static const bool kMicUseRight = true;
static const int kRecShift = 16;  // 與 esp32_voice_ai_robot 類似，太大聲可改 15..18

static const uint32_t SAMPLE_RATE = 16000;
static const size_t REC_CHUNK = 256;

I2SClass I2S;

static bool i2sBeginForPlayback() {
  I2S.end();
#if I2S_TEST_SHARED_BUS
  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, -1, -1);
#else
  I2S.setPins(PIN_SPK_BCLK, PIN_SPK_WS, PIN_SPK_DOUT, -1, -1);
#endif
  return I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

static bool i2sBeginForMic() {
  I2S.end();
#if I2S_TEST_SHARED_BUS
  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN);
#else
  I2S.setPins(PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_DOUT_UNUSED, PIN_MIC_DIN);
#endif
  return I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
}

static void playTone440HzMs(uint32_t duration_ms) {
  const float freq = 440.0f;
  const uint32_t samples_total = (uint32_t)((SAMPLE_RATE * duration_ms) / 1000);
  const uint32_t chunk = 256;
  int16_t buf[chunk];

  uint32_t n = 0;
  while (n < samples_total) {
    uint32_t count = (samples_total - n) > chunk ? chunk : (samples_total - n);
    for (uint32_t i = 0; i < count; i++) {
      float t = (float)(n + i) / (float)SAMPLE_RATE;
      float s = sinf(2.0f * 3.1415926f * freq * t);
      buf[i] = (int16_t)(s * 12000);
    }
    I2S.write((uint8_t*)buf, count * sizeof(int16_t));
    n += count;
  }
}

static float rmsFromInt16(const int16_t* x, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++) {
    double v = (double)x[i];
    acc += v * v;
  }
  acc /= (double)n;
  return (float)sqrt(acc);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== ESP32-S3 I2S Audio Test ===");
#if I2S_TEST_SHARED_BUS
  Serial.println("Mode: SHARED bus (BCLK/WS 與麥、喇叭共用)");
  Serial.print("Pins: BCLK="); Serial.print(PIN_I2S_BCLK);
  Serial.print(" WS="); Serial.print(PIN_I2S_WS);
  Serial.print(" DOUT(->amp)="); Serial.print(PIN_I2S_DOUT);
  Serial.print(" DIN(<-mic)="); Serial.println(PIN_I2S_DIN);
#else
  Serial.println("Mode: SPLIT (麥與喇叭各一組 BCLK/WS)");
  Serial.print("Mic: BCLK="); Serial.print(PIN_MIC_BCLK);
  Serial.print(" WS="); Serial.print(PIN_MIC_WS);
  Serial.print(" DIN="); Serial.println(PIN_MIC_DIN);
  Serial.print("Spk: BCLK="); Serial.print(PIN_SPK_BCLK);
  Serial.print(" WS="); Serial.print(PIN_SPK_WS);
  Serial.print(" DOUT="); Serial.println(PIN_SPK_DOUT);
#endif
  Serial.println("Tone -> then mic RMS (clap/talk -> RMS jumps).");

  if (!i2sBeginForPlayback()) {
    Serial.println("[ERR] I2S playback begin failed.");
    while (true) delay(1000);
  }
  Serial.println("[STEP] Playing 440Hz for 1200ms...");
  playTone440HzMs(1200);

  if (!i2sBeginForMic()) {
    Serial.println("[ERR] I2S mic begin failed.");
    while (true) delay(1000);
  }
  Serial.println("[STEP] Mic RMS every 200ms (INMP441 = 32-bit stereo read).");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();

  int32_t buf32[REC_CHUNK * 2];
  size_t got = I2S.readBytes((char*)buf32, sizeof(buf32));
  if (got == 0) {
    Serial.println("[WARN] mic read 0 bytes");
    return;
  }
  size_t frames = got / (sizeof(int32_t) * 2);
  int16_t out16[REC_CHUNK];
  for (size_t i = 0; i < frames && i < REC_CHUNK; i++) {
    int32_t l = buf32[i * 2 + 0];
    int32_t r = buf32[i * 2 + 1];
    int32_t s32 = kMicUseRight ? r : l;
    int32_t v = s32 >> kRecShift;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out16[i] = (int16_t)v;
  }
  float r = rmsFromInt16(out16, frames);
  Serial.print("mic_rms=");
  Serial.println(r, 1);
}
