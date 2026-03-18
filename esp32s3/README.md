# Magic Wand - ESP32-S3 N16R8 部署

本目錄為 **ESP32-S3 N16R8**（8MB Flash + 8MB PSRAM）的 Magic Wand 手勢辨識韌體，使用 **MPU6050** IMU 與 **TensorFlow Lite Micro**（esp-tflite-micro）。

## 硬體

- **開發板**: ESP32-S3 N16R8（或相容 ESP32-S3 板）
- **IMU**: MPU6050（I2C），預設接線：
  - SDA → GPIO 9
  - SCL → GPIO 8
  - VCC → 3.3V, GND → GND

腳位可在 `main/app_config.h` 中修改 `I2C_MASTER_SDA_IO`、`I2C_MASTER_SCL_IO`。

## 前置需求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v4.4 或以上
- 已安裝好 MPU6050 的 ESP32-S3 板子

## 建置與燒錄

1. **加入 TFLite Micro 依賴**（若尚未加入）：

   ```bash
   cd esp32s3
   idf.py add-dependency "espressif/esp-tflite-micro"
   ```

2. **設定目標與編譯**：

   ```bash
   idf.py set-target esp32s3
   idf.py build
   ```

3. **燒錄與監看 Serial**：

   ```bash
   idf.py -p COMx flash monitor
   ```

   請將 `COMx` 換成實際埠號（例如 Windows `COM3`，Linux `/dev/ttyUSB0`）。

## 模型

預設 `main/model_data.c` 為 **placeholder**，僅供編譯通過。實際辨識前請先訓練模型並替換：

1. 依 `train_esp32s3/README.md` 蒐集資料、訓練並匯出 **int8 量化** `.tflite`。
2. 執行：
   ```bash
   python3 train_esp32s3/tflite_to_c_array.py quantized_model.tflite esp32s3/main/model_data.c
   ```
3. 若類別數或名稱有變，請修改 `main/app_config.h` 的 `LABEL_COUNT` 與 `LABELS`。
4. 重新 `idf.py build` 並燒錄。

## 行為說明

- 上電後初始化 MPU6050 與 TFLite 解譯器。
- 主迴圈持續讀取陀螺儀與加速度計，估計重力與漂移，整合姿態並做手勢分段（等待 → 繪製 → 完成）。
- 當偵測到一筆手勢結束時，將筆畫點陣化為 32×32×3 影像，送入 TFLite 推論，並在 Serial 印出預測類別與分數。

## 目錄結構

```
esp32s3/
├── main/
│   ├── main.cpp           # 主程式、手勢邏輯、TFLite 推論
│   ├── app_config.h       # 腳位、常數、標籤
│   ├── imu_mpu6050.c/h    # MPU6050 I2C 驅動
│   ├── rasterize_stroke.c/h # 筆畫點陣化
│   ├── model_data.c/h     # 模型 C 陣列（訓練後替換）
│   └── idf_component.yml  # 依賴 esp-tflite-micro
├── CMakeLists.txt
├── sdkconfig.defaults     # ESP32-S3、PSRAM 等預設
└── README.md              # 本檔
```
