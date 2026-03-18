"""
Railway + MongoDB 後端範例
接收 ESP32 POST 的 JSON，寫入 MongoDB，並回傳反饋給裝置。
環境變數：MONGODB_URI（Railway 後台或 .env 設定）
"""

import os
from datetime import datetime
from flask import Flask, request, jsonify
from pymongo import MongoClient
from pymongo.errors import ConnectionFailure

app = Flask(__name__)

# MongoDB 連線（Railway 後台設 MONGODB_URI；資料庫預設 magic_wand）
MONGODB_URI = os.environ.get("MONGODB_URI", "mongodb://localhost:27017")
DB_NAME = os.environ.get("MONGODB_DB", "magic_wand")
COLLECTION_NAME = "device_readings"

try:
    client = MongoClient(MONGODB_URI)
    client.admin.command("ping")
    db = client[DB_NAME]
    collection = db[COLLECTION_NAME]
except ConnectionFailure as e:
    print("MongoDB 連線失敗:", e)
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
            "POST /api/data": "上傳裝置資料（AI 結果等）",
            "GET /api/health": "健康檢查",
        },
    })


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


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=(os.environ.get("FLASK_DEBUG") == "1"))
