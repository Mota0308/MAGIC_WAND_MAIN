# ESP32 POST 範例（上傳到 Railway + MongoDB）

將 AI 辨識結果以 JSON POST 到 Railway 後端，並依回傳的 `feedback` 做輸出（例如 LED）。

## 必要程式庫

- **不需額外程式庫**（已改為手動組 JSON；僅需 ESP32 核心內建 **WiFi / HTTPClient / WiFiClientSecure**）

## 修改設定

在 `esp32_post_example.ino` 中修改：

| 變數 | 說明 |
|------|------|
| `WIFI_SSID` | 你的 WiFi 名稱 |
| `WIFI_PASS` | WiFi 密碼 |
| `API_URL` | Railway 部署後的 API 網址，例如 `https://你的專案.up.railway.app/api/data` |
| `DEVICE_ID` | 裝置 ID（自訂） |

## POST 的 JSON 格式（與後端約定）

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

| 欄位 | 說明 |
|------|------|
| device_id | 裝置 ID |
| label | AI 辨識結果（例如手勢 "0"~"9"） |
| score | 信心分數 |
| sensor | 類型，如 "gesture" |
| timestamp | Unix 時間（選填） |
| extra | 其他自訂欄位（選填） |

## 後端回傳範例（ESP32 可解析 feedback）

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

- `feedback.action`：例如 `"led_on"` / `"led_off"` / `"none"`，可依此控制 GPIO。
- `feedback.message`：文字訊息，可顯示或記錄。

## 與你的 AI 整合

在實際專案中，不要用固定 `label = "3"`，改為：

- 在 **loop()** 裡當「AI 辨識到一次手勢」時再呼叫一次 POST。
- 把 **AI 輸出的類別** 填進 `doc["label"]`，**信心分數** 填進 `doc["score"]`。
- 其餘欄位同上，即可把每次辨識結果送上雲端並依 `feedback` 輸出。
