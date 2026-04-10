# ESP32 語音 AI 機器人（INMP441 → STT → Chat → TTS → MAX98357A）

## 效果

用麥克風講話 → ESP32 把錄音上傳雲端做 STT → 拿文字去問 AI → 把 AI 回覆轉成 TTS 並播放。

## 需求

- 硬體：ESP32-S3、INMP441、MAX98357A、喇叭
- Arduino-ESP32 3.x（提供 `ESP_I2S`）
- 後端：已部署 `cloud_backend`（含 `/api/stt`、`/api/chat`、`/api/tts`）
- 若要真實 STT：Railway 環境變數設定 `OPENAI_API_KEY`

## 接線（你目前的腳位）

- BCLK = GPIO14
- WS/LRCK = GPIO13
- DOUT（到 MAX98357A DIN）= GPIO11
- DIN（從 INMP441 DOUT）= GPIO12

注意：
- INMP441 VDD 必須 3.3V
- MAX98357A 若有 SD 腳，請拉到 3.3V

## 使用

1. 複製 `wifi_secrets.example.h` → `wifi_secrets.h`，填 WiFi
2. 上傳 `esp32_voice_ai_robot.ino`
3. Serial Monitor 115200，輸入 `r` 然後 Enter
4. 你會看到：
   - 錄音 bytes
   - STT 文字
   - AI 回覆文字
   - 播放 TTS

## 調整

- `REC_SECONDS`：錄音秒數（預設 4 秒）
- 若要「自動偵測你開始/停止說話（VAD）」：後續可加能量閾值 + 靜音計時

