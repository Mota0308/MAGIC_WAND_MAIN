# Magic Wand - 訓練與部署 (ESP32-S3 N16R8)

本目錄說明如何**蒐集資料**、**訓練模型**、並將模型轉成 C 陣列供 `esp32s3/` 韌體使用。

## 一、資料蒐集

兩種方式任選其一：

### 方式 A：使用原有 Magic Wand 網頁 + Arduino Nano BLE Sense

1. 用 Arduino Nano 33 BLE Sense 上傳本專案根目錄的 `magic_wand.ino`。
2. 打開 `website/index.html`，透過 BLE 連線，揮動手勢並標註。
3. 下載 `wanddata.json`。此 JSON 格式與下方訓練腳本相容。

### 方式 B：使用 ESP32-S3 + MPU6050 自行上傳

1. 先完成 `esp32s3/` 的編譯與燒錄（可先用 placeholder 模型）。
2. 撰寫一個「資料蒐集」專用韌體：讀取 MPU6050，把手勢區段透過 **Serial** 或 **BLE** 傳到電腦，存成與 `wanddata.json` 相同格式的 JSON（見下方格式說明）。

### wanddata.json 格式

與原 Magic Wand 一致，例如：

```json
{
  "strokes": [
    {
      "label": "0",
      "strokePoints": [
        {"x": 0.12, "y": -0.05},
        ...
      ]
    }
  ]
}
```

- `strokes`: 陣列，每個元素一筆手勢。
- `label`: 字串，類別（如 "0", "1", ..., "9"）。
- `strokePoints`: 陣列，每個點 `{ "x": float, "y": float }`，x/y 約在 -1～1 之間（與原專案一致）。

## 二、訓練模型（Colab 或本機）

### 使用 Colab（建議）

1. 開啟原專案訓練筆記本：  
   [Magic Wand Training Colab](https://colab.research.google.com/github/petewarden/magic_wand/blob/main/train/train_magic_wand_model.ipynb)
2. 上傳你的 `wanddata.json`（或合併多個 JSON），依筆記本步驟執行。
3. 下載產生的 **量化模型**（例如 `quantized_model.tfl` 或 `quantized_model.tflite`）。

### 本機訓練

- **腳本**：可執行 `train_esp32s3/train_model.py`（依賴：`tensorflow`, `numpy`, `scikit-learn`）：
  ```bash
  pip install tensorflow numpy scikit-learn
  python3 train_esp32s3/train_model.py path/to/wanddata.json
  ```
  會產生 `saved_model/`、`quantized_model.tflite`。
- 或參考專案根目錄的 `train/train_magic_wand_model.ipynb`，用相同步驟與資料格式訓練，並匯出 **int8 量化** 的 `.tflite`。

模型輸入須為：

- **形狀**: `(1, 32, 32, 3)`
- **型別**: int8

與 `esp32s3/main/app_config.h` 中的 `RASTER_*` 及 TFLite 設定一致。

## 三、轉成 C 陣列並部署到 ESP32-S3

1. 取得訓練好的 `.tflite` 檔案（例如 `quantized_model.tflite`）。

2. 執行轉換腳本，寫入 `esp32s3` 專案的 `main` 目錄：

   ```bash
   python3 train_esp32s3/tflite_to_c_array.py quantized_model.tflite esp32s3/main/model_data.c
   ```

3. 若類別或名稱有改動，請編輯 `esp32s3/main/app_config.h`：
   - `LABEL_COUNT`
   - `LABELS` 巨集（例如 `"0", "1", ...`）

4. 編譯並燒錄 ESP32-S3：

   ```bash
   cd esp32s3
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COMx flash monitor
   ```

   請將 `COMx` 換成實際序列埠（Windows 為 `COM3` 等，Linux/macOS 為 `/dev/ttyUSB0` 等）。

## 四、目錄結構摘要

```
magic_wand-main/
├── esp32s3/                 # ESP32-S3 N16R8 韌體
│   ├── main/
│   │   ├── model_data.c      # ← 由 tflite_to_c_array.py 覆寫
│   │   ├── app_config.h      # 腳位、LABEL_COUNT、LABELS
│   │   └── ...
│   └── ...
├── train_esp32s3/
│   ├── README.md             # 本說明
│   └── tflite_to_c_array.py  # .tflite → model_data.c
└── train/                    # 原 Colab 筆記本
    └── train_magic_wand_model.ipynb
```

## 五、常見問題

- **編譯時找不到 TFLite**  
  在 `esp32s3` 目錄執行：`idf.py add-dependency "espressif/esp-tflite-micro"`（若使用舊版 IDF，可改在 `main/idf_component.yml` 已指定依賴後重新 build）。

- **IMU 讀不到**  
  確認 MPU6050 接線與 `app_config.h` 中的 `I2C_MASTER_SDA_IO`、`I2C_MASTER_SCL_IO` 一致；N16R8 上可先用 8、9，依板子手冊調整。

- **模型太大或推論崩潰**  
  確認使用 **int8 量化** 模型，且 `TENSOR_ARENA_SIZE`（在 `app_config.h`）足夠（例如 30*1024）；必要時可再加大。
