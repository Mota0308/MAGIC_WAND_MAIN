# 將小智 AI 機器人（xiaozhi-esp32）加入 ESP32

本說明介紹如何在 **ESP32-S3**（或其它支援型號）上運行 **xiaozhi-esp32** 小智 AI 語音機器人，並可選地將語音/對話事件上報到既有 **Railway + MongoDB** 後端。

---

## 一、重要區別

| 項目 | 小智 xiaozhi-esp32 | 本專案 ESP32 POST 範例 |
|------|--------------------|-------------------------|
| 框架 | **ESP-IDF**（C++） | **Arduino** |
| 用途 | 語音喚醒、ASR、LLM、TTS、MCP 控制 | 定時 POST 感測/手勢資料到 Railway |
| 建置 | `idf.py`（需 ESP-IDF 5.4+） | Arduino IDE / PlatformIO |

- **小智** 是完整固件，燒錄後裝置即為「AI 語音機器人」。
- 若要在**同一塊板子**上同時跑小智 + 定時 POST，有兩種做法：
  1. **只燒小智固件**：用下方「可選：雲端橋接」把語音/對話事件 POST 到 Railway。
  2. **兩塊板子**：一塊燒小智、一塊燒 Arduino POST 範例。

---

## 二、小智專案位置與結構

- 專案路徑（依你本機）：  
  `c:\Users\Dolphin\Downloads\xiaozhi-esp32-main\xiaozhi-esp32-main\`  
  或從 GitHub: [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- 入口：`main/main.cc` → `Application::Initialize()` / `Run()`
- 通訊：WebSocket 或 MQTT+UDP，連到小智伺服器（預設 xiaozhi.me 或自建）。
- 支援晶片：ESP32、ESP32-S3、ESP32-C3、ESP32-P4 等；**ESP32-S3** 為常見選擇。

---

## 三、在 ESP32-S3 上建置與燒錄小智

### 1. 環境

- **ESP-IDF v5.4 或以上**（建議用官方安裝器或 docs 說明安裝）。
- 依小智文件安裝依賴（如 **esp-sr**、**xiaozhi-fonts** 等 component）。
- 本專案為 **ESP-IDF 專案**，不能用 Arduino IDE 直接開。

### 2. 選板子

小智用 `BOARD` 選開發板，ESP32-S3 常見選項例如：

- `m5stack-core-s3` — M5Stack CoreS3  
- `lichuang-dev` — 立創 ESP32-S3 開發板  
- `esp-box-3` — Espressif ESP32-S3-BOX3  
- `atoms3r-echo-base` — M5Stack AtomS3R + Echo Base  

若你的是「通用 ESP32-S3 開發板」，可先選一個腳位相近的（如 `m5stack-core-s3` 或 `lichuang-dev`）測試，或參考 [自訂開發板](https://github.com/78/xiaozhi-esp32/blob/main/docs/custom-board.md) 做一個專用板型。

### 3. 建置與燒錄（在 xiaozhi-esp32 專案根目錄）

```bash
cd path/to/xiaozhi-esp32-main

# 設定目標晶片
idf.py set-target esp32s3

# 選板子（依實際板子改）
idf.py -DBOARD=m5stack-core-s3 reconfigure

# 選單設定（WiFi、語言、資產等）
idf.py menuconfig

# 編譯
idf.py build

# 燒錄（請依你的 COM 埠改）
idf.py -p COMx flash monitor
```

初次使用可依 [小智新手燒錄指南](https://ccnphfhqs21z.feishu.cn/wiki/Zpz4wXBtdimBrLk25WdcXzxcnNS) 用現成固件先跑通，再改為自建。

### 4. 伺服器與帳號

- 預設連 **xiaozhi.me**：可先註冊帳號，依文件在裝置上完成綁定。  
- 自建伺服器：參考 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 等，再在 menuconfig 或設定中改為自建位址。

---

## 四、可選：將小智事件上報到 Railway（Magic Wand 後端）

若希望小智的 **語音辨識結果（stt）**、**TTS 句子開始（tts）** 或 **喚醒詞** 也送到你現有的 Railway 後端（`/api/data`），可在 xiaozhi-esp32 裡加一個「雲端橋接」小模組。

### 4.1 概念

- 在 `Application::InitializeProtocol()` 裡，`protocol_->OnIncomingJson(...)` 會收到伺服器下發的 JSON（type 含 `stt`、`tts`、`llm` 等）。
- 在處理到 `stt` / `tts` 時，多呼叫一個「雲端橋接」函式，用 **esp_http_client** 對 Railway 發 **POST**，格式與現有 `esp32_post_example.ino` 一致，例如：

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

後端已支援 `device_id`、`label`、`sensor`、`timestamp`、`extra` 等，無需改後端即可寫入 MongoDB。

### 4.2 本專案提供的附加檔

在 **magic_wand-main** 專案中已提供可選的橋接程式與整合說明，方便你複製進 xiaozhi 專案使用：

- **esp32_client/xiaozhi_cloud_bridge/**  
  - `cloud_bridge.h` / `cloud_bridge.cc`：用 ESP-IDF `esp_http_client` 非同步 POST 到可設定 URL。  
  - `README.md`：如何把此模組加入 xiaozhi、在 `application.cc` 哪裡呼叫、以及 Kconfig 選項（例如 `CONFIG_MAGIC_WAND_CLOUD_BRIDGE`、`CONFIG_MAGIC_WAND_CLOUD_URL`、`CONFIG_MAGIC_WAND_DEVICE_ID`）。

你只要：

1. 把 `xiaozhi_cloud_bridge` 複製到 xiaozhi 的 `main/` 下。  
2. 在 `main/CMakeLists.txt` 加入 `cloud_bridge.cc` 與 `esp_http_client`。  
3. 在 `main/Kconfig.projbuild` 加入雲端橋接的選項與預設 URL、device_id。  
4. 在 `application.cc` 的 `OnIncomingJson` 裡，在處理 `stt` / `tts` 的分支中呼叫 `CloudBridge::SendEvent(...)`（詳見該目錄 README）。

這樣小智在運行時就會把對應事件 POST 到你的 Railway，與現有 magic_wand 資料流共用同一個後端與 MongoDB。

---

## 五、與本專案其它部分的關係

- **Magic Wand 手勢**：在 **Arduino Nano 33 BLE Sense (Rev2)** 上跑 `magic_wand` 辨識，若需要把手勢結果也送到 Railway，可再搭一個 ESP32（跑 Arduino POST 範例）或其它方式上傳。  
- **Railway 後端**：`cloud_backend/` 的 `POST /api/data` 已可接受多種 `sensor`（如 `gesture`、`xiaozhi`），無需為小智改後端。  
- **ESP32 POST 範例**：`esp32_client/esp32_post_example/` 維持為 **Arduino** 範例，用來示範定時 POST 與 feedback（如 LED）；與小智是**不同固件**，可擇一燒錄或分開兩塊板子使用。

若你希望「只做一件事」：**在 ESP32-S3 上跑小智 AI 機器人**，只要完成「三、建置與燒錄」即可；若要再與 Magic Wand 雲端整合，再依「四、可選：雲端橋接」加上橋接即可。
