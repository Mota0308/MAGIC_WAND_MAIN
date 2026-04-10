# ESP8266 UDP Server (receive ON/OFF)

This is a minimal UDP receiver for testing wireless commands sent from an ESP32.

## Wiring

- This sketch toggles `RELAY_PIN` (default: `D1` on NodeMCU/Wemos D1 mini).
- Connect your LED/relay input to that pin (and GND).

## Steps

1. Copy `wifi_secrets.example.h` to `wifi_secrets.h` and fill:
   - `WIFI_SSID`
   - `WIFI_PASS`
2. Flash `esp8266_udp_server.ino` to ESP8266.
3. Open Serial Monitor at **115200** and note the IP address printed.
4. Flash the ESP32 sender sketch (`../esp32_udp_client/esp32_udp_client.ino`) and set `ESP8266_IP` to this IP.

## Protocol

- UDP port: `4210`
- Payload:
  - `ON`
  - `OFF`
- Replies:
  - `OK ON`
  - `OK OFF`

