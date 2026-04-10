/*
 * ESP32-S3 I2S audio hardware test (Arduino-ESP32 3.x / ESP_I2S.h)
 *
 * Wiring (your current plan):
 *   BCLK = GPIO14
 *   LRCLK/WS = GPIO13
 *   DOUT (to MAX98357A DIN) = GPIO11
 *   DIN  (from INMP441 DOUT) = GPIO12
 *
 * What it does:
 *   - On boot: plays a short 440Hz tone to confirm speaker path (MAX98357A + speaker).
 *   - Then: reads mic samples and prints RMS level to Serial (clap/talk -> RMS jumps).
 *
 * Notes:
 *   - INMP441 VDD must be 3.3V (NOT 5V).
 *   - MAX98357A VIN can be 3.3V–5V depending on module; you use 5V.
 *   - If your MAX98357A has SD pin, tie SD to 3.3V to ensure enabled.
 */

#include <Arduino.h>
#include <ESP_I2S.h>

// ---- Pin mapping (change if you remap GPIOs) ----
static const int PIN_I2S_BCLK = 14;
static const int PIN_I2S_WS   = 13;
static const int PIN_I2S_DOUT = 11;  // ESP32 -> amp (MAX98357A DIN)
static const int PIN_I2S_DIN  = 12;  // mic (INMP441 DOUT) -> ESP32

// ---- Audio config ----
static const uint32_t SAMPLE_RATE = 16000;
static const i2s_data_bit_width_t BITS = I2S_DATA_BIT_WIDTH_16BIT;
static const i2s_slot_mode_t CH = I2S_SLOT_MODE_MONO;

I2SClass I2S;

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
      buf[i] = (int16_t)(s * 12000);  // safe volume
    }
    I2S.write((uint8_t*)buf, count * sizeof(int16_t));
    n += count;
  }
}

static float rms16(const int16_t* x, size_t n) {
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
  Serial.println("Tone test (MAX98357A) then mic RMS (INMP441).");

  // Configure pins and start I2S in STD mode.
  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN);
  bool ok = I2S.begin(I2S_MODE_STD, SAMPLE_RATE, BITS, CH);
  if (!ok) {
    Serial.println("[ERR] I2S.begin failed. Check pins and Arduino-ESP32 version.");
    while (true) delay(1000);
  }

  Serial.println("[OK] I2S started.");
  Serial.println("[STEP] Playing 440Hz tone for 1200ms...");
  playTone440HzMs(1200);
  Serial.println("[STEP] Tone done. Now printing mic RMS every 200ms.");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();

  // Read a small chunk from mic.
  const size_t n = 512;
  int16_t buf[n];
  size_t got = I2S.readBytes((char*)buf, n * sizeof(int16_t));
  if (got == 0) {
    Serial.println("[WARN] mic read 0 bytes");
    return;
  }
  size_t samples = got / sizeof(int16_t);
  float r = rms16(buf, samples);
  Serial.print("mic_rms=");
  Serial.println(r, 1);
}

