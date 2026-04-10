# ESP32 文字輸入 → AI → TTS 播放（不需要 STT / OpenAI）

這個版本先滿足你說的「輸入為文字、輸出為 TTS」：

- Serial Monitor 輸入文字
- 雲端 `/api/chat` 回覆文字
- 雲端 `/api/tts` 產生 MP3 代理 URL
- ESP32 下載 MP3 到 RAM，使用 `ESP_I2S.playMP3()` 播放到 MAX98357A

## 需求

- Arduino-ESP32 3.x（提供 `ESP_I2S`）
- MAX98357A I2S 功放 + 喇叭
- 後端已部署（`/api/chat`、`/api/tts` 可用）

## 接線（MAX98357A）

- BCLK = GPIO14
- WS/LRCK = GPIO13
- DIN = GPIO11

## 設定

複製 `wifi_secrets.example.h` → `wifi_secrets.h`，填 WiFi。

## 使用

1. 上傳 `esp32_serial_chat_tts.ino`
2. Serial Monitor 115200
3. 輸入一行文字按 Enter
4. 你會看到 AI 回覆文字，並且喇叭會播放 TTS

