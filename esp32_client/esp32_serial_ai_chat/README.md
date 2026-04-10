# ESP32 Serial → 雲端 AI 聊天（簡易測試）

## 效果

1. 燒錄本 sketch 到 ESP32（有 WiFi 即可，不需麥克風）。
2. 開 Serial Monitor（115200，換行 NL 或 NL+CR）。
3. 輸入一行中文或英文，按 Enter。
4. ESP32 把內容 POST 到雲端，雲端運算後把結果印回 Serial。

## 網頁版（同一後端）

部署後瀏覽器開：**`https://你的網域/chat`** 即可文字聊天（與本 sketch 共用 `/api/chat`）。

## WiFi 設定

- 複製 `wifi_secrets.example.h` 為 **`wifi_secrets.h`** 並填入（已加入 `.gitignore`）。
- 若沒有 `wifi_secrets.h`，會使用 `.ino` 內預設常數。

## 雲端設定（Railway）

1. 部署 `cloud_backend`（與現有 Magic Wand 同一專案即可）。
2. **可選**：在 Railway 新增環境變數  
   - `OPENAI_API_KEY` = 你的 OpenAI API Key  
   - （可選）`OPENAI_MODEL` = `gpt-4o-mini`（預設即此）
3. 未設定 `OPENAI_API_KEY` 時，仍會回 **模擬模式** 訊息，可先用來確認 WiFi + HTTPS + 後端是否正常。

## API

`POST /api/chat`

```json
{ "device_id": "esp32_001", "message": "你好，請介紹自己" }
```

回應：

```json
{ "ok": true, "reply": "……", "mode": "openai" }
```

成功對話可選寫入 MongoDB 集合 `chat_logs`（若 MongoDB 已連線）。

## 本機測試後端

```bash
cd cloud_backend
set OPENAI_API_KEY=sk-...
python app.py
```

PowerShell 測試：

```powershell
Invoke-RestMethod -Uri "http://127.0.0.1:5000/api/chat" -Method Post -ContentType "application/json" -Body '{"message":"1+1=?"}'
```
