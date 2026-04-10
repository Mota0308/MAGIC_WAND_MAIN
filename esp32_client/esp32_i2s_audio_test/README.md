# ESP32-S3 I2S 音訊硬體測試（INMP441 + MAX98357A）

## 用途

先確認接線與硬體正常：

1. 開機先播 **440Hz 測試音**（驗證 MAX98357A + 喇叭）
2. 然後每 200ms 在 Serial 印 **mic RMS**（拍手/說話數值會跳，驗證 INMP441）

## 接線（你目前使用）

- **BCLK** = GPIO14
- **LRCLK/WS** = GPIO13
- **DOUT（ESP32 → MAX98357A DIN）** = GPIO11
- **DIN（INMP441 DOUT → ESP32）** = GPIO12

### INMP441
- VDD → **3.3V**（不要接 5V）
- GND → GND
- SCK/BCLK → GPIO14
- WS/LRCLK → GPIO13
- SD/DOUT → GPIO12
- L/R → GND（固定一邊即可）

### MAX98357A
- Vin → 5V（或 3.3V，依模組）
- GND → GND
- BCLK → GPIO14
- LRC → GPIO13
- DIN → GPIO11
- 喇叭接模組上的 **+ / -** 兩個大焊盤
- 若有 **SD** 腳，建議先拉到 **3.3V**（確保功放啟用）

## 使用方法

1. Arduino IDE 打開 `esp32_i2s_audio_test.ino`
2. 選擇你的 ESP32-S3 board + Port
3. Upload
4. Serial Monitor 設 **115200**

成功現象：
- 開機會聽到一段 440Hz 聲音
- Serial 會不停印 `mic_rms=...`，拍手/說話會變大

