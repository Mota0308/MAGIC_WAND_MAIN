/*
 * ESP8266 LED quick test (no network)
 *
 * Purpose:
 *  - Verify wiring + pin mapping by blinking an external LED.
 *  - Optional: control ON/OFF from Serial Monitor.
 *
 * Default pin:
 *  - NodeMCU / Wemos D1 mini: GPIO5 is usually labeled "D1"
 *
 * Wiring for external LED (ESP8266 only default):
 *  - GPIO5 (D1) -> 220~470Ω resistor -> LED anode (long leg)
 *  - LED cathode (short leg) -> GND
 *
 * ESP32 / ESP32-C3:
 *  - Default: uses **onboard LED** (LED_BUILTIN, often GPIO8 on Espressif C3 DevKit), **active LOW**.
 *  - If you use a breadboard LED, change LED_PIN below to your GPIO and set kLedActiveLow = false.
 *
 * ESP32-C3 (USB):
 *  - If Serial Monitor is EMPTY: Arduino IDE -> Tools -> USB CDC On Boot -> **Enabled**
 *    (then re-upload). Optional: USB Mode = Hardware CDC and JTAG.
 *
 * Serial Monitor:
 *  - 115200 baud
 *  - Type:
 *      1  => LED ON
 *      0  => LED OFF
 *      b  => toggle blink mode
 *      s  => dump diagnostics (pin read, reset reason, etc.)
 */

#include <Arduino.h>

#if defined(ESP8266)
static const int LED_PIN = 5;  // GPIO5 (D1)
static const bool kLedActiveLow = false;
#else
// ESP32-C3: default to an **external** LED on a safe GPIO.
// Recommended: GPIO4 (free on many C3 dev boards).
// If you wire to a different pin, change this number.
static const int LED_PIN = 4;
static const bool kLedActiveLow = false;  // external LED: HIGH = ON
#endif

static bool g_blinkMode = true;
static bool g_ledOn = false;
static unsigned long g_lastToggleMs = 0;
static const unsigned long BLINK_INTERVAL_MS = 500;

static void dumpDiagnostics() {
  Serial.println();
  Serial.println("----- [LOG] diagnostics -----");
#if defined(ESP8266)
  Serial.print("[LOG] reset_reason= ");
  Serial.println(ESP.getResetReason());
  Serial.print("[LOG] chip_id= 0x");
  Serial.println(ESP.getChipId(), HEX);
  Serial.print("[LOG] flash_size= ");
  Serial.print(ESP.getFlashChipSize() / 1024);
  Serial.println(" KB");
#elif defined(ESP32)
  Serial.print("[LOG] chip_model= ");
  Serial.println(ESP.getChipModel());
  Serial.print("[LOG] flash_size= ");
  Serial.print(ESP.getFlashChipSize() / 1024);
  Serial.println(" KB");
#endif
#if defined(LED_BUILTIN)
  Serial.print("[LOG] LED_BUILTIN(GPIO)= ");
  Serial.println(LED_BUILTIN);
#endif
  Serial.print("[LOG] LED_PIN GPIO = ");
  Serial.println(LED_PIN);
  Serial.print("[LOG] active_low = ");
  Serial.println(kLedActiveLow ? "true (LOW = ON)" : "false (HIGH = ON)");
  Serial.print("[LOG] pin readback = ");
  Serial.println(digitalRead(LED_PIN));
#if defined(ESP8266)
  Serial.println("[LOG] Expect: external LED + resistor from D1(GPIO5) to GND");
#else
  Serial.println("[LOG] ESP32: default targets **onboard** LED; external LED needs LED_PIN + active_low edit");
#endif
  Serial.println("[LOG] If read != last write: wrong pin, short, or dead GPIO");
  Serial.println("------------------------------");
  Serial.println();
}

static void setLed(bool on) {
  g_ledOn = on;
  int level = kLedActiveLow ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(LED_PIN, level);
  delay(2);  // let line settle for readback
  int readback = digitalRead(LED_PIN);

  Serial.print("[LED] ");
  Serial.print(on ? "ON " : "OFF");
  Serial.print("| GPIO");
  Serial.print(LED_PIN);
  Serial.print(kLedActiveLow ? " active-LOW" : " active-HIGH");
  Serial.print(" write=");
  Serial.print(level);
  Serial.print(" read=");
  Serial.print(readback);
  if (readback != level) {
    Serial.print("  [WARN] read!=write (check wiring/short/wrong pin)");
  }
  Serial.println();
}

#if defined(ESP8266)
// NodeMCU / D1 mini: built-in LED is usually GPIO2 (D4), ACTIVE LOW (LOW = on)
static void builtinLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

static void bootBlinkBuiltin() {
  pinMode(LED_BUILTIN, OUTPUT);
  builtinLed(false);
  for (int i = 0; i < 6; i++) {
    builtinLed(true);
    delay(120);
    builtinLed(false);
    delay(120);
  }
}
#elif defined(ESP32) && defined(LED_BUILTIN)
// ESP32-C3 DevKit onboard LED is often active LOW on GPIO8 (varies by board)
static void bootBlinkBuiltinEsp32() {
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(120);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(120);
  }
}
#endif

void setup() {
  Serial.begin(115200);
#if defined(ESP32)
  // Native USB (CDC): host needs time to enumerate; otherwise Serial prints are "lost"
  delay(2000);
#else
  delay(150);
#endif

#if defined(ESP8266)
  bootBlinkBuiltin();
#elif defined(ESP32) && defined(LED_BUILTIN)
  bootBlinkBuiltinEsp32();
#endif

  delay(300);
  Serial.println();
  Serial.println("========== [BOOT] LED Test starting ==========");
#if defined(ESP8266)
  Serial.println("[BOOT] Chip: ESP8266");
#else
  Serial.println("[BOOT] Chip: ESP32 family (e.g. ESP32-C3 uses GPIO below)");
#endif
  Serial.println("[BOOT] Sketch: esp8266_led_test.ino");
  Serial.print("[BOOT] LED_PIN GPIO = ");
  Serial.print(LED_PIN);
#if defined(ESP32) && defined(LED_BUILTIN)
  if (LED_PIN == LED_BUILTIN) {
    Serial.print(" (= onboard LED_BUILTIN)");
  }
#endif
  Serial.println();
  Serial.print("[BOOT] Polarity: ");
  Serial.println(kLedActiveLow ? "active-LOW (LOW = lit)" : "active-HIGH (HIGH = lit)");
  Serial.println("[BOOT] Valid Serial lines: 1=ON  0=OFF  b=blink  s=diagnostics");
  Serial.println("[BOOT] On each line you will see [RX] ... then [INPUT] OK or FAIL");
  Serial.println("[BOOT] Serial: 115200 baud, newline LF or Both NL & CR");
#if defined(ESP32)
  Serial.println("[BOOT] ESP32-C3: if this line never appears, set Tools -> USB CDC On Boot = Enabled, re-upload");
#endif
  Serial.println("===============================================");
  Serial.flush();

  pinMode(LED_PIN, OUTPUT);
  setLed(false);
  dumpDiagnostics();
  Serial.println("[BOOT] READY — type 1 / 0 / b / s + Enter");
}

void loop() {
  // Serial command handling
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    size_t rawLen = s.length();
    s.trim();

    Serial.print("[RX] raw_chars=");
    Serial.print(rawLen);
    Serial.print(" trimmed=\"");
    Serial.print(s);
    Serial.println("\"");

    if (s.length() == 0) {
      Serial.println("[INPUT] IGNORE — empty line (send 1,0,b,s only)");
      return;
    }

    if (s == "ON") {
      Serial.println("[INPUT] OK — valid command: 1 (LED ON, blink off)");
      g_blinkMode = false;
      setLed(true);
    } else if (s == "OFF") {
      Serial.println("[INPUT] OK — valid command: 0 (LED OFF, blink off)");
      g_blinkMode = false;
      setLed(false);
    } else if (s == "b" || s == "B") {
      Serial.println("[INPUT] OK — valid command: b (toggle blink mode)");
      g_blinkMode = !g_blinkMode;
      Serial.print("[MODE] blink=");
      Serial.println(g_blinkMode ? "ON" : "OFF");
    } else if (s == "s" || s == "S") {
      Serial.println("[INPUT] OK — valid command: s (diagnostics)");
      dumpDiagnostics();
    } else {
      Serial.print("[INPUT] FAIL — unknown command (expected 1,0,b,s): \"");
      Serial.print(s);
      Serial.println("\"");
    }
  }

  // Blink mode
  if (!g_blinkMode) return;
  unsigned long now = millis();
  if (now - g_lastToggleMs >= BLINK_INTERVAL_MS) {
    g_lastToggleMs = now;
    setLed(!g_ledOn);
  }
}

