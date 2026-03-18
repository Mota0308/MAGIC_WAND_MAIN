# 把 MAGIC_WAND（含 cloud_backend）上傳到 GitHub

照下面做，讓 GitHub 上有 **cloud_backend**，之後在 Railway 重新建專案時才能選到 Root Directory。

---

## 一、在 GitHub 建新 repo

1. 登入 https://github.com
2. 右上 **+** → **New repository**
3. 名稱填：`magic_wand` 或 `MAGIC_WAND_MAIN`（自訂）
4. 選 **Public**，**不要**勾 "Add a README"
5. 點 **Create repository**

---

## 二、在本機專案資料夾用 Git 上傳

請在 **PowerShell 或命令提示字元** 裡操作，路徑改成你電腦的實際位置。

### 1. 進入「內層」magic_wand-main（裡面要有 cloud_backend）

```powershell
cd C:\Users\Dolphin\Downloads\magic_wand-main\magic_wand-main
```

（若你的路徑不同，就改成你放專案的那一層，要確保這一層底下有 **cloud_backend** 資料夾。）

### 2. 若還沒用過 Git，先初始化

```powershell
git init
```

### 3. .gitignore

專案裡已有 **.gitignore**（會忽略 `__pycache__`、`.env` 等），若沒有可手動建一個再執行下一步。

### 4. 加入所有檔案並提交

```powershell
git add .
git commit -m "Add magic_wand project and cloud_backend for Railway"
```

### 5. 連到你在 GitHub 剛建好的 repo（網址改成你的）

```powershell
git remote add origin https://github.com/你的帳號/magic_wand.git
```

（例如：`https://github.com/chenyaolin0308/magic_wand.git`）

### 6. 推送到 GitHub

```powershell
git branch -M main
git push -u origin main
```

（若 GitHub 要你登入，用瀏覽器或 Personal Access Token 完成驗證。）

---

## 三、這樣一來，GitHub 上的結構會是

```
你的 repo（例如 magic_wand）
├── cloud_backend/      ← Railway 要用的
│   ├── app.py
│   ├── requirements.txt
│   ├── Procfile
│   └── ...
├── magic_wand/
├── website/
├── train/
└── ...
```

---

## 四、在 Railway 重新建專案

1. Railway → **New Project**
2. 選 **Deploy from GitHub repo**
3. 選你剛推送的 **magic_wand**（或 MAGIC_WAND_MAIN）repo
4. 部署完成後，點進該 **Service** → **Settings**
5. **Root Directory** 填：**`cloud_backend`**（不要加 `/`）
6. 儲存後會重新部署
7. 在 **Variables** 加上 **MONGODB_URI** 和 **MONGODB_DB**（見 RAILWAY_DEPLOY.md）

這樣 Railway 就會從你 GitHub 的 **cloud_backend** 部署，不再出現「Could not find root directory」的錯誤。
