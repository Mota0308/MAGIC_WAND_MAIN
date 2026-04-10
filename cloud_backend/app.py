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
import base64
import io
import concurrent.futures
from datetime import datetime
from flask import Flask, request, jsonify, render_template
from pymongo import MongoClient
from urllib.parse import urlencode
from werkzeug.middleware.proxy_fix import ProxyFix

app = Flask(__name__)
# Trust Railway/Proxy headers so request.host_url uses https.
app.wsgi_app = ProxyFix(app.wsgi_app, x_proto=1, x_host=1)

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
            "POST /api/stt": "語音（WAV）→ 文字（OpenAI STT）",
            "POST /api/command": "文字 → 家居控制指令（action/device）",
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


@app.route("/api/log", methods=["POST"])
def log_interaction():
    """
    Store one interaction (text in/out) into MongoDB.
    JSON: { "device_id": "...", "input_text": "...", "output_text": "...", "kind": "command|chat|other" }
    """
    if client is None:
        return jsonify({"ok": False, "error": "MongoDB not connected"}), 503

    body = request.get_json(force=True, silent=True)
    if not body:
        return jsonify({"ok": False, "error": "Invalid JSON"}), 400

    device_id = (body.get("device_id") or "unknown").strip()[:128]
    input_text = (body.get("input_text") or "").strip()
    output_text = (body.get("output_text") or "").strip()
    kind = (body.get("kind") or "other").strip()[:32]

    if not input_text:
        return jsonify({"ok": False, "error": "input_text is empty"}), 400
    if not output_text:
        return jsonify({"ok": False, "error": "output_text is empty"}), 400

    # Limit sizes to protect DB
    if len(input_text) > 4000:
        input_text = input_text[:4000]
    if len(output_text) > 4000:
        output_text = output_text[:4000]

    try:
        col = client[DB_NAME]["interaction_logs"]
        r = col.insert_one(
            {
                "device_id": device_id,
                "kind": kind,
                "input_text": input_text,
                "output_text": output_text,
                "ts": datetime.utcnow(),
            }
        )
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500

    return jsonify({"ok": True, "id": str(r.inserted_id)}), 201


# /api/chat：請模型在回覆最後一行輸出 DEVICE_CMD（供 ESP32 解析）；亦會寫入 JSON 欄位 device_cmd
CHAT_SYSTEM_IOT = """你是簡短、友善的助手，用繁體中文回答，盡量精簡。
使用者可能透過物聯網裝置控制一顆可調亮度的 LED 燈（亮度僅能設為 0%、25%、50%、75%、100%）。
請先給使用者自然、有幫助的回覆；然後在**最後單獨一行**（這行之後不要再輸出任何字）輸出下列格式之一：
DEVICE_CMD: ON
DEVICE_CMD: OFF
DEVICE_CMD: NONE
DEVICE_CMD: B0
DEVICE_CMD: B25
DEVICE_CMD: B50
DEVICE_CMD: B75
DEVICE_CMD: B100
規則：
- 想開燈到最亮、或沒指定亮度 → ON（等同 100%）；或明確要 100% → B100。
- 想關燈 → OFF；或要 0% 亮度 → B0。
- 使用者明確說出「25%/一半略暗/四分之一亮度」等 → 對應 B25/B50/B75（依語意選最接近的一檔）。
- 一般聊天或與燈無關 → NONE。
- 若使用者問「有哪些設備/裝置」「可連接哪些」「列出設備」等（僅查詢、不控制燈），請依下方「已登記清單」用繁體中文簡短列出名稱與 IP，然後 DEVICE_CMD: NONE。"""


_VALID_DEVICE_CMDS = frozenset(
    {"ON", "OFF", "NONE", "B0", "B25", "B50", "B75", "B100"}
)


def _iot_system_prompt(udp_targets: list | None) -> str:
    """若 ESP32 傳入 udp_targets，追加清單說明；多臺時再加選目標格式。"""
    if not udp_targets:
        return CHAT_SYSTEM_IOT
    rows: list[tuple[str, str]] = []
    for t in udp_targets:
        if not isinstance(t, dict):
            continue
        name = str(t.get("name") or "").strip()[:64]
        ip = str(t.get("ip") or "").strip()[:64]
        if name and ip:
            rows.append((name, ip))
    if not rows:
        return CHAT_SYSTEM_IOT
    lines_txt = "\n".join(f"- {n}（{ipa}）" for n, ipa in rows)
    extra = f"""

目前本裝置已登記、可透過 UDP 控制的目標如下（使用者若僅詢問「有哪些設備」請據此列出，DEVICE_CMD: NONE）：
{lines_txt}"""
    if len(rows) > 1:
        extra += f"""

目前已登記多個燈控裝置（請依使用者語意判斷要控制哪一台）：
當使用者要控制「特定那一台」時，請在 DEVICE_CMD 那一行的**正上方**多輸出一行：
DEVICE_TARGET: <名稱，須與上面列表中的名稱一致>
若使用者未指定哪一台、或無法判斷、或與燈無關、或語意是「全部」時，輸出：
DEVICE_TARGET: NONE
若只輸出 DEVICE_CMD 而未輸出 DEVICE_TARGET，裝置會沿用目前選中的目標。"""
    return CHAT_SYSTEM_IOT + extra


def _split_device_reply(reply: str) -> tuple:
    """回傳 (給使用者看的純文字, device_cmd, device_target 名稱或空字串表示不換目標)。"""
    if not reply or not reply.strip():
        return "", "NONE", ""
    lines = reply.strip().splitlines()
    device_cmd = "NONE"
    device_target = ""
    idx = len(lines) - 1
    if idx >= 0:
        last = lines[idx].strip()
        up = last.upper()
        if up.startswith("DEVICE_CMD:"):
            part = last.split(":", 1)[1].strip().upper()
            if part in _VALID_DEVICE_CMDS:
                device_cmd = part
                idx -= 1
    if idx >= 0:
        prev = lines[idx].strip()
        pup = prev.upper()
        if pup.startswith("DEVICE_TARGET:"):
            raw = prev.split(":", 1)[1].strip()
            if raw.upper() == "NONE" or not raw:
                device_target = ""
            else:
                device_target = raw[:128]
            idx -= 1
    clean = "\n".join(lines[: idx + 1]).strip()
    return (clean if clean else "（無文字內容）"), device_cmd, device_target


def _split_device_cmd_line(reply: str) -> tuple:
    """相容舊邏輯：僅解析 DEVICE_CMD。"""
    c, cmd, _ = _split_device_reply(reply)
    return c, cmd


def _heuristic_device_cmd(user_msg: str) -> str:
    """使用者文字簡易規則（模型未遵守格式時的後備）。"""
    s = (user_msg or "").strip()
    t = s.lower().replace(" ", "")
    # 亮度檔位（與 ESP32-C3 UDP B0/B25/... 一致）
    if "100%" in s or "最亮" in s or "全亮" in s:
        return "B100"
    if "最暗" in s or "全暗" in s:
        return "B0"
    if "75%" in s or "七成五" in s:
        return "B75"
    if "50%" in s or "一半" in s or "五成" in s:
        return "B50"
    if "25%" in s or "二成五" in s:
        return "B25"
    if any(x in s for x in ("關燈", "關掉", "熄燈")) or "turnoff" in t:
        return "OFF"
    if any(x in s for x in ("開燈", "打開", "亮燈", "點亮", "點燈")) or "turnon" in t:
        return "ON"
    if t.startswith("關") and "燈" in s:
        return "OFF"
    if t.startswith("開") and "燈" in s:
        return "ON"
    return "NONE"


def _heuristic_device_target(user_msg: str, udp_targets: list | None) -> str:
    """模擬模式：若訊息含某台名稱子字串，回傳該名稱；否則空字串。"""
    if not udp_targets or len(udp_targets) <= 1:
        return ""
    s = user_msg or ""
    best = ""
    best_len = 0
    for t in udp_targets:
        if not isinstance(t, dict):
            continue
        name = str(t.get("name") or "").strip()
        if len(name) >= 1 and name in s and len(name) > best_len:
            best = name
            best_len = len(name)
    return best


def _load_recent_chat_history(device_id: str, max_turns: int, max_docs_cap: int = 0) -> list[dict]:
    """
    從 MongoDB chat_logs 讀取對話紀錄（每筆含 user_message + ai_reply），組成 OpenAI messages。
    不包含 system 與「本次」user 訊息。

    max_turns:
      - > 0 : 只取時間上最新的 max_turns 筆（與先前行為一致）
      - 0   : 「盡可能讀全部」— 見 max_docs_cap
      - < 0 : 不載入歷史

    max_docs_cap（僅在 max_turns==0 時有效）:
      - 0   : 不限制筆數（該 device_id 在 chat_logs 裡全部讀出；資料量大時可能超時或超模型 token）
      - > 0 : 只取「最新」這麼多筆（等同全部裡的安全上限）
    """
    if client is None:
        return []
    if max_turns < 0:
        return []
    did = (device_id or "serial_chat").strip()[:128]
    try:
        chat_col = client[DB_NAME]["chat_logs"]
        proj = {"user_message": 1, "ai_reply": 1, "timestamp": 1}
        if max_turns > 0:
            cur = chat_col.find({"device_id": did}, proj).sort("timestamp", -1).limit(int(max_turns))
            docs = list(cur)
            docs.reverse()  # oldest -> newest
        else:
            # max_turns == 0：讀全部或「最新 N 筆」上限
            if max_docs_cap and max_docs_cap > 0:
                cur = chat_col.find({"device_id": did}, proj).sort("timestamp", -1).limit(int(max_docs_cap))
                docs = list(cur)
                docs.reverse()
            else:
                cur = chat_col.find({"device_id": did}, proj).sort("timestamp", 1)
                docs = list(cur)
    except Exception:
        return []
    msgs: list[dict] = []
    for d in docs:
        um = (d.get("user_message") or "").strip()
        ar = (d.get("ai_reply") or "").strip()
        if um:
            msgs.append({"role": "user", "content": um[:8000]})
        if ar:
            msgs.append({"role": "assistant", "content": ar[:8000]})
    return msgs


def _build_chat_messages(device_id: str, user_text: str, system_prompt: str | None) -> list[dict]:
    sys_content = system_prompt or "你是簡短、友善的助手，用繁體中文回答，盡量精簡。"
    max_turns = int(os.environ.get("CHAT_MEMORY_TURNS", "8") or "8")
    # CHAT_MEMORY_MAX_DOCS：CHAT_MEMORY_TURNS=0 時，最多載入幾筆（0=不限制＝真的全撈）
    max_docs_cap = int(os.environ.get("CHAT_MEMORY_MAX_DOCS", "0") or "0")
    history = _load_recent_chat_history(
        device_id, max_turns=max_turns, max_docs_cap=max_docs_cap
    )
    return [{"role": "system", "content": sys_content}] + history + [{"role": "user", "content": (user_text or "")[:8000]}]


def _openai_chat_messages(messages: list[dict]) -> str:
    """呼叫 OpenAI Chat Completions（messages 版）；失敗時拋出例外。"""
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        raise ValueError("OPENAI_API_KEY not set")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    payload = json_lib.dumps({"model": model, "messages": messages}).encode("utf-8")
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


def _poe_chat_messages(messages: list[dict]) -> str:
    """呼叫 Poe Chat Completions（OpenAI-compatible；messages 版）；失敗時拋出例外。"""
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")
    model = os.environ.get("POE_MODEL", "gpt-4o-mini")
    payload = json_lib.dumps({"model": model, "messages": messages, "stream": False}).encode("utf-8")
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


def _openai_chat(user_text: str, system_prompt: str | None = None) -> str:
    """呼叫 OpenAI Chat Completions；失敗時拋出例外。"""
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        raise ValueError("OPENAI_API_KEY not set")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    sys_content = system_prompt or "你是簡短、友善的助手，用繁體中文回答，盡量精簡。"
    payload = json_lib.dumps(
        {
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": sys_content,
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

def _poe_chat(user_text: str, system_prompt: str | None = None) -> str:
    """呼叫 Poe Chat Completions（OpenAI-compatible）；失敗時拋出例外。"""
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")
    # Poe uses bot names as model id. You may need to change this in Railway variables.
    model = os.environ.get("POE_MODEL", "gpt-4o-mini")
    sys_content = system_prompt or "你是簡短、友善的助手，用繁體中文回答，盡量精簡。"
    payload = json_lib.dumps(
        {
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": sys_content,
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

def _poe_command(user_text: str, devices: list[str]) -> dict:
    """
    Use Poe chat completions to convert natural language into a command JSON.
    Returns a dict with keys: action, device, say (optional), confidence (optional).
    """
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")
    model = os.environ.get("POE_MODEL", "gpt-4o-mini")
    devices_str = ", ".join(devices) if devices else "light1"
    sys = (
        "你是一個家居控制指令解析器。"
        "請把使用者輸入解析為 JSON，且只能輸出 JSON，不要輸出其他文字。"
        "JSON 格式固定為："
        '{"action":"on|off|toggle|status|unknown","device":"<device>","say":"<繁體中文回覆，可選>","confidence":0.0}'
        "其中 device 必須從允許清單中挑一個；若沒提到就用 light1。"
        f"允許的 device 清單：{devices_str}。"
        "若是聊天或無法判斷，action=unknown，confidence=0。"
    )
    payload = json_lib.dumps(
        {
            "model": model,
            "stream": False,
            "messages": [
                {"role": "system", "content": sys},
                {"role": "user", "content": user_text[:2000]},
            ],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        "https://api.poe.com/v1/chat/completions",
        data=payload,
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        data = json_lib.loads(resp.read().decode("utf-8"))
    raw = (data["choices"][0]["message"]["content"] or "").strip()
    if not raw:
        return {"action": "unknown", "device": "light1", "say": "", "confidence": 0.0}
    # Best-effort: model might wrap JSON in code fences
    txt = raw
    if "```" in txt:
        txt = txt.replace("```json", "").replace("```", "").strip()
    try:
        obj = json_lib.loads(txt)
    except Exception:
        return {"action": "unknown", "device": "light1", "say": "", "confidence": 0.0}
    if not isinstance(obj, dict):
        return {"action": "unknown", "device": "light1", "say": "", "confidence": 0.0}
    action = str(obj.get("action") or "unknown").lower().strip()
    device = str(obj.get("device") or "light1").strip()
    say = str(obj.get("say") or "").strip()
    try:
        conf = float(obj.get("confidence") or 0.0)
    except Exception:
        conf = 0.0
    if devices and device not in devices:
        device = devices[0]
    if action not in ("on", "off", "toggle", "status", "unknown"):
        action = "unknown"
    if conf < 0.0:
        conf = 0.0
    if conf > 1.0:
        conf = 1.0
    return {"action": action, "device": device, "say": say, "confidence": conf}


@app.route("/api/command", methods=["POST"])
def command():
    """
    Natural language -> structured device command.
    JSON: { "text": "幫我開燈", "device_id": "optional" }
    Returns: { ok, action, device, say, confidence, mode }
    """
    body = request.get_json(force=True, silent=True)
    if not body:
        return jsonify({"ok": False, "error": "Invalid JSON"}), 400
    text = (body.get("text") or body.get("message") or "").strip()
    if not text:
        return jsonify({"ok": False, "error": "text is empty"}), 400
    if len(text) > 2000:
        text = text[:2000]

    devices_env = os.environ.get("COMMAND_DEVICES", "light1,light2").strip()
    devices = [d.strip() for d in devices_env.split(",") if d.strip()]
    if not devices:
        devices = ["light1"]

    try:
        provider = os.environ.get("AI_PROVIDER", "").strip().lower()
        if not provider:
            if os.environ.get("OPENAI_API_KEY", "").strip():
                provider = "openai"
            elif os.environ.get("POE_API_KEY", "").strip():
                provider = "poe"
        if provider == "poe":
            cmd = _poe_command(text, devices)
            mode = "poe"
        else:
            # Fallback: simple keyword rules if no Poe configured
            t = text.lower()
            action = "unknown"
            if "off" in t or "關" in text:
                action = "off"
            elif "on" in t or "開" in text:
                action = "on"
            cmd = {"action": action, "device": devices[0], "say": "", "confidence": 0.0}
            mode = "rule"
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 502

    return jsonify(
        {
            "ok": True,
            "action": cmd.get("action", "unknown"),
            "device": cmd.get("device", devices[0]),
            "say": cmd.get("say", ""),
            "confidence": cmd.get("confidence", 0.0),
            "mode": mode,
        }
    ), 200

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

def _encode_multipart_form(fields, files):
    """
    Minimal multipart/form-data encoder for urllib.
    fields: list of (name, value_str)
    files: list of (name, filename, content_type, bytes)
    """
    boundary = "----MagicWandBoundary" + secrets.token_hex(12)
    crlf = "\r\n"
    body = bytearray()

    for name, value in fields:
        body.extend(("--" + boundary + crlf).encode("utf-8"))
        body.extend((f'Content-Disposition: form-data; name="{name}"' + crlf + crlf).encode("utf-8"))
        body.extend((str(value) + crlf).encode("utf-8"))

    for name, filename, content_type, data in files:
        body.extend(("--" + boundary + crlf).encode("utf-8"))
        body.extend((f'Content-Disposition: form-data; name="{name}"; filename="{filename}"' + crlf).encode("utf-8"))
        body.extend((f"Content-Type: {content_type}" + crlf + crlf).encode("utf-8"))
        body.extend(data)
        body.extend(crlf.encode("utf-8"))

    body.extend(("--" + boundary + "--" + crlf).encode("utf-8"))
    return boundary, bytes(body)


def _openai_stt_wav(wav_bytes: bytes) -> str:
    """Call OpenAI audio transcriptions API with a WAV file; return text."""
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        raise ValueError("OPENAI_API_KEY not set")

    model = os.environ.get("OPENAI_STT_MODEL", "whisper-1").strip() or "whisper-1"
    language = os.environ.get("OPENAI_STT_LANGUAGE", "").strip()  # optional: "zh"

    fields = [("model", model)]
    if language:
        fields.append(("language", language))
    # "response_format": "json" is default
    boundary, body = _encode_multipart_form(
        fields=fields,
        files=[("file", "audio.wav", "audio/wav", wav_bytes)],
    )

    req = urllib.request.Request(
        "https://api.openai.com/v1/audio/transcriptions",
        data=body,
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        data = json_lib.loads(resp.read().decode("utf-8"))
    text = (data.get("text") or "").strip()
    if not text:
        raise ValueError("STT returned empty text")
    return text

def _poe_stt_wav(wav_bytes: bytes) -> str:
    """
    Call Poe bot (e.g. Whisper-V3-Large-T) via fastapi-poe with file upload.
    Returns transcript text.
    """
    key = os.environ.get("POE_API_KEY", "").strip()
    if not key:
        raise ValueError("POE_API_KEY not set")

    # Poe bot name as used in Poe UI (public bots only)
    bot = os.environ.get("POE_STT_MODEL", "Whisper-V3-Large-T").strip() or "Whisper-V3-Large-T"

    try:
        import fastapi_poe as fp
    except Exception as e:
        raise RuntimeError("fastapi-poe is not installed") from e

    # Upload WAV as an attachment
    f = io.BytesIO(wav_bytes)
    f.name = "audio.wav"  # some libs expect a name attribute
    att = fp.upload_file_sync(f, api_key=key)

    msg = fp.ProtocolMessage(
        role="user",
        content=(
            "請把這段音訊『逐字轉寫成繁體中文』，"
            "不要翻譯成其他語言，也不要總結或改寫，只輸出聽到的內容本身，"
            "不加任何說明或標註。"
        ),
        attachments=[att],
    )

    parts = []
    for partial in fp.get_bot_response_sync(messages=[msg], bot_name=bot, api_key=key):
        if isinstance(partial, str):
            parts.append(partial)
        else:
            # Best-effort: some versions may yield objects with .text
            t = getattr(partial, "text", None)
            if isinstance(t, str):
                parts.append(t)
    text = "".join(parts).strip()
    if not text:
        raise ValueError("Poe STT returned empty text")
    return text


def _run_with_timeout(fn, timeout_sec: float):
    """
    Run a blocking function with a wall-clock timeout.
    This prevents requests from hanging until the gunicorn worker timeout.
    """
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as ex:
        fut = ex.submit(fn)
        return fut.result(timeout=timeout_sec)


@app.route("/api/chat", methods=["POST"])
def chat():
    """
    ESP32 Serial 測試用：上傳使用者文字，雲端運算後回傳 AI 回覆。
    JSON: { "message": "你好", "device_id": "esp32_001", "include_tts": true,
            "udp_targets": [ {"name":"客廳","ip":"192.168.1.10"}, ... ] }（後兩者可選）
    多個 UDP 目標時，模型可輸出 DEVICE_TARGET（上一行）+ DEVICE_CMD；回應 JSON 含 device_target。
    include_tts=true 且已設定 POE_API_KEY 時，同時回傳 tts_absolute_url，省裝置再 POST /api/tts。
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
    want_inline_tts = bool(body.get("include_tts") or body.get("tts"))
    udp_targets = body.get("udp_targets")
    if not isinstance(udp_targets, list):
        udp_targets = None
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

        sys_iot = _iot_system_prompt(udp_targets)
        if provider == "openai":
            messages = _build_chat_messages(device_id, msg, sys_iot)
            reply = _openai_chat_messages(messages)
            mode = "openai"
        elif provider == "poe":
            messages = _build_chat_messages(device_id, msg, sys_iot)
            reply = _poe_chat_messages(messages)
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
        hcmd = _heuristic_device_cmd(msg)
        htarget = _heuristic_device_target(msg, udp_targets)
        tail = ""
        if udp_targets and len(udp_targets) > 1 and htarget:
            tail = f"\nDEVICE_TARGET: {htarget}"
        reply = (
            f"[模擬模式] 已收到你的內容（前 80 字）：{msg[:80]}"
            + ("…" if len(msg) > 80 else "")
            + "。請在 Railway 環境變數設定 OPENAI_API_KEY 以啟用真實 AI 回覆。\n"
            + tail
            + f"\nDEVICE_CMD: {hcmd}"
        )
        mode = "mock"

    reply_clean, device_cmd, device_target = _split_device_reply(reply)
    if device_cmd == "NONE" and mode != "mock":
        device_cmd = _heuristic_device_cmd(msg)

    # 可選：寫入 MongoDB（與 device_readings 分開）
    if collection is not None:
        try:
            chat_col = client[DB_NAME]["chat_logs"]
            doc = {
                "device_id": device_id,
                "user_message": msg,
                "ai_reply": reply_clean,
                "device_cmd": device_cmd,
                "mode": mode,
                "timestamp": datetime.utcnow(),
            }
            if device_target:
                doc["device_target"] = device_target
            chat_col.insert_one(doc)
        except Exception:
            pass

    out: dict = {
        "ok": True,
        "reply": reply_clean,
        "device_cmd": device_cmd,
        "device_target": device_target,
        "mode": mode,
    }
    # 一次請求內完成 TTS，省 ESP32 再 POST /api/tts 的往返延遲（需 POE_API_KEY）
    if want_inline_tts and os.environ.get("POE_API_KEY", "").strip():
        try:
            tts_max = int(os.environ.get("CHAT_TTS_MAX_CHARS", "400") or "400")
            if tts_max < 50:
                tts_max = 50
            if tts_max > 2000:
                tts_max = 2000
            tts_text = (reply_clean or "")[:tts_max]
            if tts_text.strip():
                upstream_url = _poe_tts_url(tts_text)
                token = secrets.token_urlsafe(10)
                _tts_cache[token] = (upstream_url, time.time())
                proxy_url = f"/api/tts/audio/{token}"
                out["tts_absolute_url"] = request.host_url.rstrip("/") + proxy_url
        except Exception as e:
            out["tts_error"] = str(e)

    return jsonify(out), 200


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


@app.route("/api/stt", methods=["POST"])
def stt():
    """
    Speech-to-text (WAV -> text).

    Accepts:
    - Raw WAV bytes with Content-Type: audio/wav (recommended for ESP32)
    - JSON: { "wav_base64": "...", "device_id": "optional" }
    """
    wav_bytes = b""
    ct = (request.headers.get("Content-Type") or "").lower()
    if ct.startswith("audio/"):
        wav_bytes = request.data or b""
    else:
        body = request.get_json(force=True, silent=True) or {}
        b64 = (body.get("wav_base64") or "").strip()
        if b64:
            try:
                wav_bytes = base64.b64decode(b64, validate=True)
            except Exception:
                return jsonify({"ok": False, "error": "Invalid wav_base64"}), 400

    if not wav_bytes or len(wav_bytes) < 44:
        return jsonify({"ok": False, "error": "Missing WAV audio"}), 400

    try:
        # Prefer Poe STT if configured; else fall back to OpenAI STT.
        if os.environ.get("POE_API_KEY", "").strip():
            text = _poe_stt_wav(wav_bytes)
            mode = "poe"
        else:
            text = _openai_stt_wav(wav_bytes)
            mode = "openai"
    except Exception as e:
        # No key or upstream error -> mock (keeps the pipeline testable)
        text = "[模擬STT] 已收到音訊（bytes=%d）。請在 Railway 設定 OPENAI_API_KEY 以啟用真實語音辨識。" % len(wav_bytes)
        mode = "mock"
        # If key exists but call fails, return 502 with reason.
        if os.environ.get("OPENAI_API_KEY", "").strip() or os.environ.get("POE_API_KEY", "").strip():
            return jsonify({"ok": False, "error": str(e)}), 502

    return jsonify({"ok": True, "text": text, "mode": mode}), 200


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
