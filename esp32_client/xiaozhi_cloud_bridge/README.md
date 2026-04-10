# 小智 xiaozhi-esp32 雲端橋接（可選）

把 xiaozhi-esp32 的語音事件（stt / tts / 喚醒）以 POST 送到 Magic Wand 的 Railway 後端（`/api/data`），寫入 MongoDB。

## 使用方式

將本目錄的 `cloud_bridge.h`、`cloud_bridge.cc` 複製到 **xiaozhi-esp32** 的 `main/` 目錄下，然後依下列步驟修改小智專案。

### 1. 在 xiaozhi-esp32 的 `main/Kconfig.projbuild` 末尾加入

```kconfig
config MAGIC_WAND_CLOUD_BRIDGE
    bool "Enable Magic Wand cloud bridge (POST xiaozhi events to Railway)"
    default n
    help
        When enabled, STT/TTS and optional wake events are POSTed to the URL below.

config MAGIC_WAND_CLOUD_URL
    string "Magic Wand cloud API URL"
    default "https://magicwandmain-production.up.railway.app/api/data"
    depends on MAGIC_WAND_CLOUD_BRIDGE
    help
        Full URL for POST /api/data.

config MAGIC_WAND_DEVICE_ID
    string "Device ID for cloud"
    default "xiaozhi_esp32_001"
    depends on MAGIC_WAND_CLOUD_BRIDGE
    help
        device_id sent in JSON body.
```

### 2. 在 xiaozhi-esp32 的 `main/CMakeLists.txt` 中

- 在 `set(SOURCES ...)` 的列表裡加入：`"cloud_bridge.cc"`
- 在 `idf_component_register(... PRIV_REQUIRES ...)` 的 **PRIV_REQUIRES** 裡加入：`esp_http_client`

### 3. 在 `application.cc` 中

- 檔案頂部加入：`#include "cloud_bridge.h"`
- 在 `HandleNetworkConnectedEvent()` 裡、網路連線成功後呼叫：`CloudBridge::Init();`
- 在 `protocol_->OnIncomingJson([this, display](const cJSON* root) { ... });` 的回調裡：
  - 處理 **stt** 時（`strcmp(type->valuestring, "stt") == 0`），在顯示用戶語句後加一行：  
    `CloudBridge::SendEvent("stt", text->valuestring);`
  - 處理 **tts** 的 `sentence_start` 時（有 `text`），在顯示助手語句後加一行：  
    `CloudBridge::SendEvent("tts", text->valuestring);`
- （可選）在 **喚醒詞** 觸發處（例如 `HandleWakeWordDetectedEvent` 或 `WakeWordInvoke` 被呼叫時）加：  
  `CloudBridge::SendEvent("wake", wake_word.c_str());`

### 4. menuconfig

建置前執行 `idf.py menuconfig`，在 **Xiaozhi Assistant** 下可看到：

- **Enable Magic Wand cloud bridge** — 選 Y 啟用
- **Magic Wand cloud API URL** — 你的 Railway `/api/data` 網址
- **Device ID for cloud** — 裝置 ID（會出現在 MongoDB 的 `device_id`）

POST 的 JSON 格式與 `esp32_post_example.ino` 相容，例如：

```json
{
  "device_id": "xiaozhi_esp32_001",
  "label": "stt",
  "score": 0,
  "sensor": "xiaozhi",
  "timestamp": 1234567890,
  "extra": { "text": "用戶說的話" }
}
```

後端會寫入 MongoDB，無需改動 `cloud_backend`。
