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
import time
import secrets
from datetime import datetime
from flask import Flask, request, jsonify, render_template
from pymongo import MongoClient
from urllib.parse import urlencode

app = Flask(__name__)

# In-memory short link store: id -> (upstream_url, created_ts)
# Good enough for testing; redeploy will clear it.
_tts_cache = {}
_tts_cache_ttl_sec = 15 * 60

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
            "POST /api/tts": "文字 → 語音（Poe TTS：回傳音檔 URL）",
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

def _poe_chat(user_text: str) -> str:
    """呼叫 Poe Chat Completions（OpenAI-compatible）；失敗時拋出例外。"""
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")
    # Poe uses bot names as model id. You may need to change this in Railway variables.
    model = os.environ.get("POE_MODEL", "gpt-4o-mini")
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
            "stream": False,
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        "https://api.poe.com/v1/chat/completions",
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

def _poe_tts_url(text: str) -> str:
    """呼叫 Poe TTS，回傳音檔 URL（目前已驗證 message.content 會是 URL）。"""
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")
    model = os.environ.get("POE_TTS_MODEL", "Gemini-2.5-Pro-TTS")
    payload = json_lib.dumps(
        {
            "model": model,
            "stream": False,
            "messages": [
                {"role": "system", "content": "請把使用者輸入轉成自然語音輸出。"},
                {"role": "user", "content": text[:2000]},
            ],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        "https://api.poe.com/v1/chat/completions",
        data=payload,
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        data = json_lib.loads(resp.read().decode("utf-8"))
    url = data["choices"][0]["message"]["content"]
    if not isinstance(url, str) or not url.startswith("http"):
        raise ValueError("Poe TTS did not return an audio URL in message.content")
    return url.strip()


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
        provider = os.environ.get("AI_PROVIDER", "").strip().lower()
        if not provider:
            # Backward-compatible default: prefer OpenAI if configured; else Poe.
            if os.environ.get("OPENAI_API_KEY", "").strip():
                provider = "openai"
            elif os.environ.get("POE_API_KEY", "").strip():
                provider = "poe"

        if provider == "openai":
            reply = _openai_chat(msg)
            mode = "openai"
        elif provider == "poe":
            reply = _poe_chat(msg)
            mode = "poe"
    except Exception as e:
        return jsonify(
            {
                "ok": False,
                "error": str(e),
                "hint": "檢查 AI_PROVIDER、OPENAI_API_KEY/POE_API_KEY、以及 Railway 是否可連外（api.openai.com / api.poe.com）",
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


@app.route("/api/tts", methods=["POST"])
def tts():
    """
    文字轉語音：回傳可下載的音檔 URL。
    JSON: { "text": "你好", "device_id": "optional" }
    """
    body = request.get_json(force=True, silent=True)
    if not body:
        return jsonify({"ok": False, "error": "Invalid JSON"}), 400
    text = (body.get("text") or body.get("message") or "").strip()
    if not text:
        return jsonify({"ok": False, "error": "text is empty"}), 400

    try:
        upstream_url = _poe_tts_url(text)
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 502

    # Some PoeCDN URLs deny direct browser/IoT downloads and the long query can break some ESP32 URL parsers.
    # Provide a short proxy URL hosted by this backend: /api/tts/audio/<id>
    token = secrets.token_urlsafe(10)
    _tts_cache[token] = (upstream_url, time.time())
    proxy_url = f"/api/tts/audio/{token}"
    absolute_proxy_url = request.host_url.rstrip("/") + proxy_url
    return jsonify({
        "ok": True,
        "provider": "poe",
        "upstream_url": upstream_url,
        "url": proxy_url,
        "absolute_url": absolute_proxy_url,
        "note": "Use `absolute_url` for ESP32 playback (short proxy).",
    }), 200


def _tts_cache_get(token: str):
    item = _tts_cache.get(token)
    if not item:
        return None
    upstream_url, created = item
    if (time.time() - created) > _tts_cache_ttl_sec:
        _tts_cache.pop(token, None)
        return None
    return upstream_url


@app.route("/api/tts/audio/<token>", methods=["GET"])
def tts_audio_token(token: str):
    """
    Proxy audio download to bypass upstream access restrictions.
    Path: /api/tts/audio/<token> (token maps to upstream_url in cache)
    Returns: audio bytes with Content-Type forwarded when possible.
    """
    upstream_url = _tts_cache_get(token)
    if not upstream_url:
        return jsonify({"ok": False, "error": "Invalid or expired token"}), 404

    try:
        # Some CDNs require a User-Agent header.
        req = urllib.request.Request(
            upstream_url,
            headers={
                "User-Agent": "Mozilla/5.0",
                "Accept": "*/*",
            },
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=90) as resp:
            content_type = resp.headers.get("Content-Type") or "application/octet-stream"
            data = resp.read()
    except urllib.error.HTTPError as e:
        return jsonify({"ok": False, "error": f"Upstream HTTP {e.code}"}), 502
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 502

    return (data, 200, {"Content-Type": content_type, "Cache-Control": "no-store"})


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=(os.environ.get("FLASK_DEBUG") == "1"))
