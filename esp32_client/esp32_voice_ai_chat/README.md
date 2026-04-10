# ESP32 語音版 AI（INMP441 + MAX98357A + Poe TTS URL）

## 目標

- 麥克風收音（INMP441）→（下一步：STT）→ 文字送到雲端 `/api/chat`
- 後端回覆文字後，再呼叫 `/api/tts` 拿到 **音檔 URL**
- ESP32 下載音檔並播放（需要可播放格式；建議 WAV/MP3 其一）

## 目前狀態（你已完成）

- I2S 硬體已測通：播音 + 收音 OK
- 後端已新增 `POST /api/tts`，回 `{ url: "https://..." }`

## 下一步（播放）

Poe 回傳的是音檔 URL（目前看起來是 `poecdn.net/.../audio/...`）。

ESP32 要播，需要其中一種：

1. **URL 是 WAV(PCM)**：可以用 Arduino-ESP32 的 I2S 直接播放 PCM（最簡單）
2. **URL 是 MP3**：需要 MP3 解碼庫（例如 ESP32-audioI2S 之類）再輸出 I2S

建議先用瀏覽器打開 URL 看它下載的是 WAV 還是 MP3，再決定用哪個播放器方案。

