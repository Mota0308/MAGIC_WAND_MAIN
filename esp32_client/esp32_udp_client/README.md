# ESP32 UDP Client (send ON/OFF to ESP8266)

This is a minimal UDP sender for testing wireless commands from an ESP32 to an ESP8266.

## Steps

1. Flash the ESP8266 server sketch first: `../esp8266_udp_server/esp8266_udp_server.ino`
2. Open ESP8266 Serial Monitor at **115200** and copy its IP address (printed as `WiFi OK. IP: ...`).
3. In this folder, copy `wifi_secrets.example.h` to `wifi_secrets.h` and fill:
   - `WIFI_SSID`
   - `WIFI_PASS`
   - `ESP8266_IP` (the IP from step 2)
4. Flash `esp32_udp_client.ino` to ESP32.
5. Open ESP32 Serial Monitor at **115200** and type:
   - `ON` + Enter
   - `OFF` + Enter

You should see `Reply: OK ON` / `Reply: OK OFF`.

