# 文字版 AI 機器人（暫不使用小智）

## 功能

| 方式 | 說明 |
|------|------|
| **瀏覽器** | 開 `https://你的網域/chat`，輸入文字與 AI 對話 |
| **ESP32 Serial** | 燒錄 `esp32_serial_ai_chat.ino`，115200 輸入一行按 Enter |

兩者都呼叫同一後端 **`POST /api/chat`**。

---

## 雲端（Railway）

1. 部署目錄：`cloud_backend`（與既有 Magic Wand 相同服務即可）。
2. 重新部署後應有：
   - `GET /chat` — 聊天頁
   - `POST /api/chat` — JSON `{ "message": "...", "device_id": "可選" }`
3. **真實 AI**：Railway 環境變數新增  
   - （選一種供應商）
     - **OpenAI**：`AI_PROVIDER=openai`、`OPENAI_API_KEY=...`、（可選）`OPENAI_MODEL=gpt-4o-mini`
     - **Poe**：`AI_PROVIDER=poe`、`POE_API_KEY=...`、（可選）`POE_MODEL=gpt-4o-mini`（Poe 使用 bot 名稱）
4. **未設 Key**：仍會回 200，內容為「模擬模式」提示，可測連線。

對話可選寫入 MongoDB 集合 **`chat_logs`**（MongoDB 已連線時）。

---

## ESP32

1. Arduino IDE 打開資料夾：`esp32_client/esp32_serial_ai_chat/`
2. **建議**：複製 `wifi_secrets.example.h` → `wifi_secrets.h`，填入 WiFi 與 `CHAT_URL`（勿把 `wifi_secrets.h` 推到公開倉庫）。
3. 若沒有 `wifi_secrets.h`，會使用 `.ino` 內預設常數（請直接改 `.ino` 裡的 `YOUR_WIFI` 等）。
4. 板型選 **ESP32 / ESP32-S3**，燒錄後 Serial Monitor **115200**，輸入訊息後送車換行。

---

## 本機測試後端

```powershell
cd cloud_backend
$env:OPENAI_API_KEY="sk-..."
python app.py
```

瀏覽器開 `http://127.0.0.1:5000/chat`。
