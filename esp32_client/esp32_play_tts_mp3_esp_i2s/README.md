# ESP32 播放 Poe TTS MP3（ESP_I2S 版本，先下載再播放）

## 為什麼要用這個版本

你目前使用 `ESP32-audioI2S` 會卡在 HTTPS/redirect/串流流程；此版本改成：

1. 先用 HTTPS 下載代理 MP3（通常約 24KB）
2. 再用 Arduino-ESP32 的 `ESP_I2S` 的 `playMP3()` 一次性播放

更適合沒有 PSRAM 的 ESP32-S3。

## 需求

- Arduino-ESP32 3.x（提供 `<ESP_I2S.h>`）

## 設定

複製 `wifi_secrets.example.h` → `wifi_secrets.h`，填：
- WiFi
- `TTS_MP3_URL`：從 `POST /api/tts` 取得的 `absolute_url`（短 token URL）

## I2S 腳位（MAX98357A）

- BCLK=GPIO14
- WS=GPIO13
- DOUT=GPIO11

