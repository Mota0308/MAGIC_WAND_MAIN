# ESP32 播放 Poe TTS MP3（MAX98357A）

## 目的

用 ESP32-audioI2S 直接播放後端 `POST /api/tts` 取得的 **代理 MP3 URL**。

## 需求

- Arduino IDE 已安裝 `ESP32-audioI2S`（schreibfaul1）

## 接線

- MAX98357A VIN → 5V
- MAX98357A GND → GND（共地）
- MAX98357A BCLK → GPIO14
- MAX98357A LRC  → GPIO13
- MAX98357A DIN  → GPIO11
- 喇叭接在模組的 `+ / -` 大焊盤
- 若有 SD 腳：SD → 3.3V（啟用）

## 設定

複製 `wifi_secrets.example.h` 為 `wifi_secrets.h`，填：
- `WIFI_SSID` / `WIFI_PASS`
- `TTS_MP3_URL`：**完整代理 URL**（網域 + `/api/tts/audio?u=...`）

## 使用

1. 打開 `esp32_play_tts_mp3.ino`
2. 選 ESP32S3 Dev Module、Flash Size 8MB（依你板子）
3. Upload
4. Serial Monitor 115200，看 log 並聽喇叭

