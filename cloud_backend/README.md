# Railway + MongoDB 後端（Python）

接收 ESP32 以 POST 上傳的 JSON，寫入 **magic_wand** 資料庫，並回傳反饋給裝置。

## 環境變數（Railway）

在 Railway 專案 → **Variables** 新增：

| 變數 | 必填 | 說明 |
|------|------|------|
| `MONGODB_URI` | ✅ | MongoDB Atlas 連線字串（從 Atlas 複製，勿寫進程式碼） |
| `MONGODB_DB` | 選填 | 資料庫名稱，不設則使用 **magic_wand** |
| `OPENAI_API_KEY` | 選填 | 設定後 **`/api/chat`** 使用真實 AI；未設則模擬回覆 |
| `OPENAI_MODEL` | 選填 | 預設 `gpt-4o-mini` |

## 文字 AI 機器人（無小智）

- **GET `/chat`** — 瀏覽器聊天頁
- **POST `/api/chat`** — JSON `{"message":"你好","device_id":"可選"}` → `reply`、`mode`

詳見專案根目錄 **`TEXT_AI_ROBOT.md`**。

## 本機測試

```bash
cd cloud_backend
pip install -r requirements.txt
# 若有本地 MongoDB：
export MONGODB_URI=mongodb://localhost:27017
python app.py
# 或無 MongoDB 時仍可跑，寫入會回 503
python app.py
```

## API

- **GET /** — 說明
- **GET /chat** — 文字 AI 聊天頁
- **GET /api/health** — 健康檢查（含 MongoDB 連線狀態）
- **POST /api/data** — ESP32 上傳資料，見下方 JSON 格式
- **POST /api/chat** — 文字 AI（見上）

## ESP32 POST 的 JSON 格式

```json
{
  "device_id": "esp32_001",
  "label": "3",
  "score": 85,
  "sensor": "gesture",
  "timestamp": 1699123456,
  "extra": {}
}
```

| 欄位 | 必填 | 說明 |
|------|------|------|
| device_id | 建議 | 裝置 ID |
| label | 建議 | AI 辨識結果（例如手勢 "0"~"9"） |
| score | 選填 | 信心分數（-128~127 或 0~100） |
| sensor | 選填 | 類型，如 "gesture" |
| timestamp | 選填 | Unix 時間，不送則伺服器自動填 |
| extra | 選填 | 其他欄位 |

回傳範例（201）：

```json
{
  "ok": true,
  "id": "...",
  "feedback": {
    "action": "led_on",
    "message": "odd"
  }
}
```

ESP32 可依 `feedback.action` / `feedback.message` 做輸出（例如開關 LED）。
