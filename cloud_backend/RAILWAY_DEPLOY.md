# Railway 部署步驟（接 MongoDB Atlas + magic_wand 資料庫）

## 一、準備

- 已有一個 **Railway** 帳號（https://railway.app）
- 已有 **MongoDB Atlas** 連線字串（格式類似 `mongodb+srv://用戶:密碼@cluster0.xxxxx.mongodb.net/?...`）
- 本專案中的 `cloud_backend` 資料夾（內含 `app.py`、`requirements.txt`、`Procfile`）

---

## 二、在 Railway 建立專案並部署

### 方式 A：從 GitHub 部署（建議）

1. 把整個專案（含 `cloud_backend`）推送到 GitHub。
2. 登入 **Railway** → **New Project**。
3. 選 **Deploy from GitHub repo**，選你的 repo。
4. Railway 會問要部署哪個目錄：
   - 若可選 **Root Directory**，設成 **`cloud_backend`**（只部署這個資料夾）。
   - 若只能選整個 repo，之後要在設定裡把 **Root Directory** 設為 `cloud_backend`。
5. 等建置完成（會自動跑 `pip install -r requirements.txt` 並用 `Procfile` 啟動）。

### 方式 B：用 Railway CLI 部署

1. 安裝 Railway CLI：https://docs.railway.app/develop/cli  
2. 終端機執行：
   ```bash
   cd magic_wand-main/cloud_backend
   railway login
   railway init
   railway up
   ```
3. 依提示建立/選擇專案，完成上傳。

---

## 三、設定 MongoDB 連線（接 magic_wand 資料庫）

1. 在 Railway 專案裡，點進你剛部署的 **Service**。
2. 點 **Variables**（或 **Settings → Environment Variables**）。
3. 新增變數：

   | 變數名稱 | 值 |
   |----------|-----|
   | `MONGODB_URI` | **貼上你的 MongoDB Atlas 連線字串**（從 Atlas 複製，含密碼） |
   | `MONGODB_DB` | `magic_wand`（要寫入的資料庫名稱；不設也會用 magic_wand） |

4. 儲存後 Railway 會自動重新部署，後端就會連到 Atlas 的 **magic_wand** 資料庫。

**注意：** 連線字串不要寫進程式碼或上傳到 GitHub，只放在 Railway 的 Variables。

---

## 四、取得 API 網址

1. 同一個 Service 裡，點 **Settings**。
2. 在 **Networking** 或 **Domains** 區塊，點 **Generate Domain**（或 **Add Domain**）。
3. 會得到一個網址，例如：`https://cloud-backend-production-xxxx.up.railway.app`
4. 你的 API 端點就是：
   - 上傳資料：`https://你的網址/api/data`
   - 健康檢查：`https://你的網址/api/health`

把 **「你的網址」** 記下來，ESP32 的 `API_URL` 就填：`https://你的網址/api/data`。

---

## 五、確認 MongoDB 使用 magic_wand 資料庫

- 後端程式預設會連到 **資料庫名稱** `magic_wand`（由 `MONGODB_DB` 決定，不設則為 `magic_wand`）。
- 資料會寫入 **magic_wand** 裡的 **device_readings** collection。
- 用 **MongoDB Compass** 連到同一個 Atlas cluster，選擇資料庫 **magic_wand**，即可看到 **device_readings** 與 ESP32 上傳的資料。

---

## 六、檢查是否成功

1. 瀏覽器打開：`https://你的網址/api/health`  
   - 應看到 `{"status":"ok","mongodb":"connected"}`。
2. 用 Postman 或 curl 測試 POST：
   ```bash
   curl -X POST https://你的網址/api/data \
     -H "Content-Type: application/json" \
     -d "{\"device_id\":\"test\",\"label\":\"1\",\"score\":80,\"sensor\":\"gesture\"}"
   ```
   - 應回傳 201 和 `feedback`。
3. 在 Compass 的 **magic_wand.device_readings** 裡應能看到一筆新文件。

完成以上步驟後，Railway 上的後端就會接上你的 MongoDB 雲端 URL，並使用 **magic_wand** 資料庫。

---

## 若出現「Application failed to respond」

1. **埠號一致**  
   Networking 裡若填 **5000**，請在 **Variables** 新增 **`PORT`** = **`5000`**（與對外埠相同），儲存後重新部署。  
   Gunicorn 會聽 `$PORT`，若 Railway 預設是 8080、你卻把網域指到 5000，就會連不上。

2. **看 Deploy Logs**  
   Service → **Deployments** → 點最新一次 → **View Logs**，看是否有 Python 錯誤或 `MongoDB`。

3. **程式已更新**  
   新版 `app.py` 在 MongoDB 連不上時仍會啟動網站（`/api/health` 可開），請把更新後的 `cloud_backend` **push 到 GitHub** 讓 Railway 重新部署。
