"""
Railway + MongoDB 後端範例
接收 ESP32 POST 的 JSON，寫入 MongoDB，並回傳反饋給裝置。
環境變數：MONGODB_URI（Railway 後台或 .env 設定）
可選：OPENAI_API_KEY → POST /api/chat 使用雲端大模型回覆
"""

import json as json_lib
import os
import urllib.error
import urllib.request
from datetime import datetime
from flask import Flask, request, jsonify, render_template
from pymongo import MongoClient

app = Flask(__name__)

# MongoDB 連線（Railway 後台設 MONGODB_URI；資料庫預設 magic_wand）
MONGODB_URI = os.environ.get("MONGODB_URI", "mongodb://localhost:27017")
DB_NAME = os.environ.get("MONGODB_DB", "magic_wand")
COLLECTION_NAME = "device_readings"

client = None
collection = None
try:
    # 逾時避免啟動卡住；連線失敗不可讓程式崩潰，否則 Railway 會顯示 Application failed to respond
    client = MongoClient(MONGODB_URI, serverSelectionTimeoutMS=8000)
    client.admin.command("ping")
    collection = client[DB_NAME][COLLECTION_NAME]
    print("MongoDB OK")
except Exception as e:
    print("MongoDB 略過（服務仍啟動）:", type(e).__name__, e)
    collection = None


def get_feedback_for_device(device_id: str, label: str, score: float) -> dict:
    """
    依 AI 辨識結果決定回傳給裝置的指令（可改成你的規則或雲端模型）。
    """
    # 範例：依手勢標籤回傳不同 action
    if score >= 50:
        if label in ("1", "3", "5", "7", "9"):
            return {"action": "led_on", "message": "odd"}
        if label in ("0", "2", "4", "6", "8"):
            return {"action": "led_off", "message": "even"}
    return {"action": "none", "message": "low_confidence"}


@app.route("/")
def index():
    return jsonify({
        "service": "ESP32 + MongoDB API",
        "endpoints": {
            "GET /chat": "瀏覽器文字 AI 聊天頁",
            "POST /api/data": "上傳裝置資料（AI 結果等）",
            "POST /api/chat": "文字 → 雲端 AI 回覆（OPENAI_API_KEY 或模擬模式）",
            "GET /api/health": "健康檢查",
        },
    })


@app.route("/chat")
def chat_page():
    """瀏覽器版文字 AI 機器人（與 ESP32 Serial 共用 /api/chat）。"""
    return render_template("chat.html")


@app.route("/api/health")
def health():
    ok = collection is not None
    return jsonify({"status": "ok" if ok else "error", "mongodb": "connected" if ok else "disconnected"})


# ---------------------------------------------------------------------------
# ESP32 上傳資料的格式範例（POST JSON）：
#
# {
#   "device_id": "esp32_001",
#   "timestamp": 1699123456,           // 可選，伺服器可自動補
#   "label": "3",                      // AI 辨識結果，例如手勢 "0"~"9"
#   "score": 85,                       // 信心分數 -128~127 或 0~100
#   "sensor": "gesture",               // 可選：gesture / imu / ...
#   "extra": {}                        // 可選：其他欄位
# }
# ---------------------------------------------------------------------------

@app.route("/api/data", methods=["POST"])
def post_data():
    if collection is None:
        return jsonify({"error": "MongoDB not connected"}), 503

    data = request.get_json(force=True, silent=True)
    if not data:
        return jsonify({"error": "Invalid JSON"}), 400

    # 必填建議：device_id, label（或你自訂欄位）
    device_id = data.get("device_id", "unknown")
    label = data.get("label", "")
    score = data.get("score", 0)
    sensor = data.get("sensor", "gesture")

    doc = {
        "device_id": device_id,
        "label": str(label),
        "score": float(score),
        "sensor": sensor,
        "timestamp": data.get("timestamp") or datetime.utcnow(),
        "extra": data.get("extra", {}),
    }
    try:
        result = collection.insert_one(doc)
        doc["_id"] = str(result.inserted_id)
    except Exception as e:
        return jsonify({"error": str(e)}), 500

    # 雲端依資料回傳反饋給 ESP32
    feedback = get_feedback_for_device(device_id, doc["label"], doc["score"])
    return jsonify({
        "ok": True,
        "id": str(result.inserted_id),
        "feedback": feedback,
    }), 201


def _openai_chat(user_text: str) -> str:
    """呼叫 OpenAI Chat Completions；失敗時拋出例外。"""
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        raise ValueError("OPENAI_API_KEY not set")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    payload = json_lib.dumps(
        {
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": "你是簡短、友善的助手，用繁體中文回答，盡量精簡。",
                },
                {"role": "user", "content": user_text[:8000]},
            ],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        "https://api.openai.com/v1/chat/completions",
        data=payload,
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        data = json_lib.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"].strip()


@app.route("/api/chat", methods=["POST"])
def chat():
    """
    ESP32 Serial 測試用：上傳使用者文字，雲端運算後回傳 AI 回覆。
    JSON: { "message": "你好", "device_id": "esp32_001" }（device_id 可選）
    """
    body = request.get_json(force=True, silent=True)
    if not body:
        return jsonify({"ok": False, "error": "Invalid JSON"}), 400
    msg = (body.get("message") or body.get("text") or "").strip()
    if not msg:
        return jsonify({"ok": False, "error": "message is empty"}), 400
    if len(msg) > 8000:
        msg = msg[:8000]

    device_id = body.get("device_id", "serial_chat")
    reply = None
    mode = "mock"

    try:
        if os.environ.get("OPENAI_API_KEY", "").strip():
            reply = _openai_chat(msg)
            mode = "openai"
    except Exception as e:
        return jsonify(
            {
                "ok": False,
                "error": str(e),
                "hint": "檢查 OPENAI_API_KEY 與 Railway 網路是否可連 api.openai.com",
            }
        ), 502

    if reply is None:
        # 無 API Key：模擬回覆，方便先測 Serial → 雲端流程
        reply = (
            f"[模擬模式] 已收到你的內容（前 80 字）：{msg[:80]}"
            + ("…" if len(msg) > 80 else "")
            + "。請在 Railway 環境變數設定 OPENAI_API_KEY 以啟用真實 AI 回覆。"
        )
        mode = "mock"

    # 可選：寫入 MongoDB（與 device_readings 分開）
    if collection is not None:
        try:
            chat_col = client[DB_NAME]["chat_logs"]
            chat_col.insert_one(
                {
                    "device_id": device_id,
                    "user_message": msg,
                    "ai_reply": reply,
                    "mode": mode,
                    "timestamp": datetime.utcnow(),
                }
            )
        except Exception:
            pass

    return jsonify({"ok": True, "reply": reply, "mode": mode}), 200


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=(os.environ.get("FLASK_DEBUG") == "1"))
