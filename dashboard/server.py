#!/usr/bin/env python3
"""
dashboard/server.py — 设备联网数据展示后端

后台线程订阅设备通过 MQTT 上传的数据（发布方见
Edgi_Talk_M55_LVGL/applications/mqtt_upload.c，topic: socc/health/<device_id>/telemetry），
写入本地 SQLite，同时用 Flask 提供网页 + REST API。核心链路不依赖任何第三方账号/云服务。

可选的「AI 健康报告」功能（/api/ai_report）会调用 DeepSeek 大模型把数据解读成
自然语言。API key 只从服务端环境变量读取，绝不下发到浏览器——前端只调本机的
代理端点。不配 key 时其余功能照常工作，只是该端点返回 503。

用法：
    pip install -r requirements.txt
    export DEEPSEEK_API_KEY=sk-xxxx     # 可选，Windows: set DEEPSEEK_API_KEY=sk-xxxx
    python server.py
浏览器打开 http://localhost:5000 ；同一局域网内的手机也可以用这台电脑的局域网 IP 访问
（如 http://192.168.1.x:5000），方便demo 时用手机看。
"""
import json
import csv
import io
import math
import os
import sqlite3
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import requests
from flask import Flask, Response, jsonify, request, render_template

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(BASE_DIR, "data", "telemetry.db")


def _load_dotenv(path):
    """极简 .env 读取（KEY=VALUE，# 起头为注释）。

    真实环境变量优先，.env 只补空缺——这样 systemd/CI 里设的值不会被文件覆盖。
    只为了少装一个 python-dotenv 依赖，功能够用即可。
    """
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = value


_load_dotenv(os.path.join(BASE_DIR, ".env"))

MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = "socc/health/+/telemetry"

ONLINE_TIMEOUT_MS = 30_000  # 超过这么久没收到新数据就判定离线（设备每 10s 发一次）
HISTORY_MAX_ROWS = 10000
HISTORY_MAX_HOURS = 168

# 和 mqtt_upload.c build_payload() 里的 JSON 字段一一对应
# 每项测量值必须带有效标志；体表红外温度不作为核心体温解释。
FIELDS = [
    "heart_rate", "spo2", "glucose", "bmr",
    "env_temp", "env_humidity", "blood_flow",
    "arr_class", "arr_valid", "arr_beat_count", "arr_pvc_per_min",
    "hrv_rmssd_ms", "hrv_valid",
    "resp_rate_bpm", "resp_valid",
    "health_score", "health_score_valid",
    "hr_valid", "spo2_valid", "glucose_valid", "bmr_valid", "env_valid",
    "blood_flow_valid", "body_temp", "body_temp_valid", "arr_source", "arr_pvc_valid",
]

# 缺少有效标志的旧固件数据按未测处理；数值合理不能代替采集有效性。
METRIC_GROUPS = {
    "hr_valid": {"heart_rate": (20, 250)},
    "spo2_valid": {"spo2": (50, 100)},
    "glucose_valid": {"glucose": (1, 35)},
    "bmr_valid": {"bmr": (1, 500)},
    "env_valid": {"env_temp": (-40, 125), "env_humidity": (0, 100)},
    "blood_flow_valid": {"blood_flow": (0, 1000)},
    "body_temp_valid": {"body_temp": (-40, 125)},
    "hrv_valid": {"hrv_rmssd_ms": (0, 500)},
    "resp_valid": {"resp_rate_bpm": (4, 60)},
    "health_score_valid": {"health_score": (0, 100)},
    "arr_valid": {"arr_class": (0, 3), "arr_beat_count": (0, 4294967295)},
    "arr_pvc_valid": {"arr_pvc_per_min": (0, 4294967295)},
}


def normalize_telemetry(payload):
    """入库与读取历史使用同一有效性规则，不修改旧数据库中的原始记录。"""
    data = dict(payload)
    for flag, metrics in METRIC_GROUPS.items():
        valid = data.get(flag) in (1, True)
        parsed = {}
        for key, (lo, hi) in metrics.items():
            value = data.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                valid = False
                continue
            try:
                ok = math.isfinite(value) and lo <= value <= hi
            except (OverflowError, TypeError):
                ok = False
            if not ok:
                valid = False
            parsed[key] = value
        data[flag] = int(valid)
        for key in metrics:
            data[key] = parsed.get(key) if valid else None
    if data.get("arr_source") not in (0, 1):
        data["arr_source"] = None
    if not data["arr_valid"]:
        data["arr_pvc_valid"] = 0
        data["arr_pvc_per_min"] = None
    return data

# ---- AI 健康报告（DeepSeek 代理）----
DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "").strip()
DEEPSEEK_BASE_URL = os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com").rstrip("/")
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")

AI_TREND_HOURS = 24          # 喂给模型的趋势统计窗口
AI_MIN_INTERVAL_S = 15       # 两次真实调用的最小间隔，防连点刷爆额度
AI_TIMEOUT = (5, 60)         # (连接, 读取) 秒；模型出一段报告通常 5-20s

# ---- 设备屏 AI 建议（下行 publish 到设备 advice topic）----
DEVICE_ADVICE_INTERVAL_S = 180   # 生成间隔（节流控成本；数据没更新还会跳过）
_mqtt_client = None              # mqtt 线程创建后赋值，供建议线程跨线程 publish
_device_advice_state = {"data_ts": None}

_db_lock = threading.Lock()


def get_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(conn):
    cols_sql = ",\n".join(f"{f} REAL" for f in FIELDS)
    conn.execute(f"""
        CREATE TABLE IF NOT EXISTS telemetry (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            ts INTEGER NOT NULL,
            {cols_sql}
        )
    """)
    existing = {r[1] for r in conn.execute("PRAGMA table_info(telemetry)")}
    for field in FIELDS:
        if field not in existing:
            conn.execute(f"ALTER TABLE telemetry ADD COLUMN {field} REAL")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_device_ts ON telemetry(device_id, ts)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(ts)")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS device_status (
            device_id TEXT PRIMARY KEY,
            last_seen INTEGER NOT NULL
        )
    """)
    conn.commit()


_conn = get_db()
init_db(_conn)

app = Flask(__name__)


# ------------------------------------------------------------------ #
# MQTT 订阅
# ------------------------------------------------------------------ #

MQTT_TOPIC_ADVICE_REQ = "socc/health/+/advice_req"


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[server] connected to {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe(MQTT_TOPIC, qos=0)
        client.subscribe(MQTT_TOPIC_ADVICE_REQ, qos=0)
        print(f"[server] subscribed {MQTT_TOPIC} + {MQTT_TOPIC_ADVICE_REQ}")
    else:
        print(f"[server] mqtt connect failed, rc={rc}")


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")
    device_id = parts[2] if len(parts) >= 3 else "unknown"

    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        print(f"[server] bad payload from {msg.topic}: {e}")
        return

    if not isinstance(payload, dict):
        return
    payload = normalize_telemetry(payload)

    device_id = payload.get("device_id", device_id)
    # 设备没有 RTC/NTP，入库时间戳由本进程（系统时间准确）打上
    ts = int(datetime.now(timezone.utc).timestamp() * 1000)

    # advice_req 与 telemetry 载荷同构（设备按键时随请求带上最新快照，
    # 不依赖"上一条 telemetry 恰好已到"的时序）——统一先入库
    cols = ["device_id", "ts"] + FIELDS
    values = [device_id, ts] + [payload.get(f) for f in FIELDS]
    placeholders = ",".join("?" * len(cols))

    with _db_lock:
        _conn.execute(
            f"INSERT INTO telemetry ({','.join(cols)}) VALUES ({placeholders})",
            values,
        )
        _conn.execute(
            "INSERT INTO device_status (device_id, last_seen) VALUES (?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen",
            (device_id, ts),
        )
        _conn.commit()

    if msg.topic.endswith("/advice_req"):
        # 设备按键请求：立即生成并推回（放一次性线程，LLM 5-15s 不能堵
        # mqtt 回调；in-flight 锁在生成函数内，狂按只跑一次）
        print(f"[server] advice request from {device_id}")
        threading.Thread(
            target=_gen_and_publish_device_advice,
            args=("button-req",), daemon=True,
        ).start()
    else:
        print(f"[server] stored telemetry from {device_id}")


def mqtt_thread_entry():
    global _mqtt_client
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    _mqtt_client = client
    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except OSError as e:
            print(f"[server] mqtt connection error: {e}, retry in 5s")
            time.sleep(5)


# ------------------------------------------------------------------ #
# Web / REST API
# ------------------------------------------------------------------ #

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/latest")
def api_latest():
    device_id = request.args.get("device_id")

    with _db_lock:
        if device_id:
            row = _conn.execute(
                "SELECT * FROM telemetry WHERE device_id=? ORDER BY ts DESC LIMIT 1",
                (device_id,),
            ).fetchone()
        else:
            row = _conn.execute(
                "SELECT * FROM telemetry ORDER BY ts DESC LIMIT 1"
            ).fetchone()

        status_row = None
        if row:
            status_row = _conn.execute(
                "SELECT * FROM device_status WHERE device_id=?", (row["device_id"],)
            ).fetchone()

    now = int(datetime.now(timezone.utc).timestamp() * 1000)
    last_seen = status_row["last_seen"] if status_row else None
    online = bool(last_seen) and (now - last_seen) < ONLINE_TIMEOUT_MS

    return jsonify({
        "telemetry": normalize_telemetry(row) if row else None,
        "online": online,
        "last_seen": last_seen,
        "server_time": now,
    })


@app.route("/api/history")
def api_history():
    try:
        _, rows = _history_rows(request.args.get("device_id"), request.args.get("hours", "24"))
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except OverflowError as e:
        return jsonify({"error": str(e)}), 413
    return jsonify(rows)


def _history_rows(device_id, hours_text):
    """固定一个设备、有限时间窗；超限报错，不能悄悄截断对照数据。"""
    try:
        hours = float(hours_text)
    except (ValueError, TypeError, OverflowError):
        raise ValueError("时间范围应为0到168小时之间的数字（不含0）。") from None
    if not math.isfinite(hours) or not 0 < hours <= HISTORY_MAX_HOURS:
        raise ValueError("时间范围应为0到168小时之间的数字（不含0）。")
    now = int(datetime.now(timezone.utc).timestamp() * 1000)
    start = now - int(hours * 3600 * 1000)
    with _db_lock:
        if not device_id:
            latest = _conn.execute("SELECT device_id FROM telemetry ORDER BY ts DESC, id DESC LIMIT 1").fetchone()
            if not latest:
                return None, []
            device_id = latest["device_id"]
        rows = _conn.execute(
            "SELECT * FROM telemetry WHERE device_id=? AND ts>=? AND ts<=? ORDER BY ts ASC, id ASC LIMIT ?",
            (device_id, start, now, HISTORY_MAX_ROWS + 1),
        ).fetchall()
    if len(rows) > HISTORY_MAX_ROWS:
        raise OverflowError("记录超过10000条，请缩短时间范围后重试。")
    return device_id, [normalize_telemetry(r) for r in rows]


@app.route("/api/devices")
def api_devices():
    with _db_lock:
        rows = _conn.execute(
            "SELECT device_id, MAX(ts) AS last_seen FROM telemetry GROUP BY device_id ORDER BY last_seen DESC, device_id"
        ).fetchall()
    now = int(datetime.now(timezone.utc).timestamp() * 1000)
    return jsonify([{"device_id": r["device_id"], "last_seen": r["last_seen"],
                     "online": 0 <= now-r["last_seen"] < ONLINE_TIMEOUT_MS} for r in rows])


def _csv_cell(value):
    # 设备标识来自网络，防止在表格软件中被解释成公式；数字仍保持数字类型。
    if isinstance(value, str) and value.lstrip().startswith(("=", "+", "-", "@")):
        return "'" + value
    return value


@app.route("/api/export.csv")
def api_export_csv():
    try:
        _, rows = _history_rows(request.args.get("device_id"), request.args.get("hours", "24"))
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except OverflowError as e:
        return jsonify({"error": str(e)}), 413
    if not rows:
        return jsonify({"error": "所选时间段没有上传记录。"}), 404
    out = io.StringIO(newline="")
    writer = csv.writer(out)
    writer.writerow(["record_id", "device_id", "received_at_utc", "received_at_ms", *FIELDS,
                     "reference_heart_rate_bpm", "reference_spo2_pct", "reference_notes"])
    for row in rows:
        when = datetime.fromtimestamp(row["ts"]/1000, timezone.utc).isoformat(timespec="milliseconds")
        writer.writerow([row["id"], _csv_cell(row["device_id"]), when, row["ts"],
                         *(row.get(f) for f in FIELDS), "", "", ""])
    response = Response("\ufeff" + out.getvalue(), content_type="text/csv; charset=utf-8")
    response.headers["Content-Disposition"] = 'attachment; filename="telemetry-reference.csv"'
    response.headers["Cache-Control"] = "no-store"
    return response


# ------------------------------------------------------------------ #
# AI 健康报告（DeepSeek 代理）
#
# 浏览器只请求本机的 /api/ai_report，key 留在服务端；前端拿不到也发不出去。
# ------------------------------------------------------------------ #

# 设备侧 arrhythmia 模型的 4 分类。按项目一贯口径：只给描述性说法，不译成病名。
ARR_CLASS_DESC = {
    0: ("N", "正常搏动"),
    1: ("S", "室上性异位搏动"),
    2: ("V", "室性异位搏动（室性早搏）"),
    3: ("F", "融合搏动"),
}

_ai_lock = threading.Lock()
_ai_state = {
    "data_ts": None,     # 生成该报告所依据的那条 telemetry 的 ts
    "device_id": None,
    "report": None,
    "generated_at": 0.0,
    "model": None,
    "last_call": 0.0,    # 上次真实打到 DeepSeek 的时刻，用于节流
}


class UpstreamError(RuntimeError):
    def __init__(self, status, detail):
        super().__init__(f"DeepSeek 返回 {status}: {detail}")
        self.status = status
        self.detail = detail


def _in_range(value, lo, hi):
    """有效标志以外的值域检查；单独使用不能证明测量有效。"""
    if value is None:
        return None
    try:
        v = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return v if lo <= v <= hi else None


def _flag(row, key):
    v = row[key] if key in row.keys() else None
    return v in (1, True)


def _gated(row, key, valid_key, lo, hi):
    """有 *_valid 标志的指标：标志为假直接判未测，再做值域兜底。"""
    if not _flag(row, valid_key):
        return None
    return _in_range(row[key], lo, hi)


def _metric(value, unit, reference=None, experimental=False):
    m = {"measured": value is not None, "unit": unit}
    if value is not None:
        m["value"] = round(value, 2)
    if reference:
        m["reference_range"] = reference
    if experimental:
        m["experimental"] = True
    return m


def _round(v, n=1):
    return None if v is None else round(float(v), n)


def _build_snapshot(device_id=None):
    """把最近一条 telemetry + 近 AI_TREND_HOURS 小时统计整理成喂模型的结构。

    返回 (snapshot, data_ts, device_id)；没有任何数据时返回 (None, None, None)。
    """
    with _db_lock:
        if device_id:
            row = _conn.execute(
                "SELECT * FROM telemetry WHERE device_id=? ORDER BY ts DESC LIMIT 1",
                (device_id,),
            ).fetchone()
        else:
            row = _conn.execute(
                "SELECT * FROM telemetry ORDER BY ts DESC LIMIT 1"
            ).fetchone()

        if row is None:
            return None, None, None

        dev = row["device_id"]
        status_row = _conn.execute(
            "SELECT * FROM device_status WHERE device_id=?", (dev,)
        ).fetchone()

        start = row["ts"] - AI_TREND_HOURS * 3600 * 1000
        agg = _conn.execute(
            """
            SELECT COUNT(*) AS n,
                   AVG(CASE WHEN hr_valid=1 AND heart_rate BETWEEN 20 AND 250 THEN heart_rate END) AS hr_avg,
                   MIN(CASE WHEN hr_valid=1 AND heart_rate BETWEEN 20 AND 250 THEN heart_rate END) AS hr_min,
                   MAX(CASE WHEN hr_valid=1 AND heart_rate BETWEEN 20 AND 250 THEN heart_rate END) AS hr_max,
                   AVG(CASE WHEN spo2_valid=1 AND spo2 BETWEEN 50 AND 100 THEN spo2 END) AS spo2_avg,
                   MIN(CASE WHEN spo2_valid=1 AND spo2 BETWEEN 50 AND 100 THEN spo2 END) AS spo2_min,
                   AVG(CASE WHEN health_score_valid=1 AND health_score BETWEEN 0 AND 100 THEN health_score END) AS score_avg,
                   MIN(CASE WHEN health_score_valid=1 AND health_score BETWEEN 0 AND 100 THEN health_score END) AS score_min,
                   AVG(CASE WHEN hrv_valid=1 AND hrv_rmssd_ms BETWEEN 0 AND 500 THEN hrv_rmssd_ms END) AS hrv_avg,
                   MAX(CASE WHEN arr_valid=1 AND arr_pvc_valid=1 AND arr_pvc_per_min BETWEEN 0 AND 4294967295 THEN arr_pvc_per_min END) AS pvc_max
            FROM telemetry WHERE device_id=? AND ts>=?
            """,
            (dev, start),
        ).fetchone()

    row = normalize_telemetry(dict(row))
    now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    last_seen = status_row["last_seen"] if status_row else row["ts"]

    hr = _gated(row, "heart_rate", "hr_valid", 20, 250)
    spo2 = _gated(row, "spo2", "spo2_valid", 50, 100)
    glucose = _gated(row, "glucose", "glucose_valid", 1, 35)
    hrv = _gated(row, "hrv_rmssd_ms", "hrv_valid", 0, 500)
    resp = _gated(row, "resp_rate_bpm", "resp_valid", 4, 60)
    score = _gated(row, "health_score", "health_score_valid", 0, 100)

    metrics = {
        "heart_rate": _metric(hr, "bpm", "60-100"),
        "spo2": _metric(spo2, "%", "95-100"),
        "hrv_rmssd": _metric(hrv, "ms", "RMSSD 20-100"),
        "respiration_rate": _metric(resp, "次/分", "12-20", experimental=True),
        "glucose": _metric(glucose, "mmol/L", "空腹 3.9-6.1", experimental=True),
        "health_score": _metric(score, "分（满分 100）", "设备规则式综合评分，非临床量表"),
    }

    arrhythmia = {"measured": False}
    if _flag(row, "arr_valid"):
        cls = int(row["arr_class"] or 0)
        letter, desc = ARR_CLASS_DESC.get(cls, ("?", "未知"))
        arrhythmia = {
            "measured": True,
            "source": {0: "设备端 NPU CNN（MIT-BIH 训练）",
                       1: "设备端 R-R 时序规则粗判，未使用 NPU 分类"}.get(row["arr_source"], "来源未记录"),
            "last_beat_class": letter,
            "last_beat_meaning": desc,
            "analyzed_beat_count": int(row["arr_beat_count"] or 0),
            "pvc_per_min": int(row["arr_pvc_per_min"]) if _flag(row, "arr_pvc_valid") else None,
            "pvc_note": "室早负荷 >6 次/分属于需要关注的范围（粗判，不是诊断）",
        }

    snapshot = {
        "device_id": dev,
        "sampled_at": datetime.fromtimestamp(row["ts"] / 1000, timezone.utc)
                              .astimezone().isoformat(timespec="seconds"),
        "data_age_seconds": max(0, (now_ms - row["ts"]) // 1000),
        "device_online": (now_ms - last_seen) < ONLINE_TIMEOUT_MS,
        "metrics": metrics,
        "arrhythmia": arrhythmia,
        "environment": {
            "temperature_c": _round(_gated(row, "env_temp", "env_valid", -40, 125)),
            "humidity_pct": _round(_gated(row, "env_humidity", "env_valid", 0, 100)),
        },
        "trend": {
            "window_hours": AI_TREND_HOURS,
            "sample_count": int(agg["n"] or 0),
            "heart_rate": {"avg": _round(agg["hr_avg"]), "min": _round(agg["hr_min"]),
                           "max": _round(agg["hr_max"])},
            "spo2": {"avg": _round(agg["spo2_avg"]), "min": _round(agg["spo2_min"])},
            "health_score": {"avg": _round(agg["score_avg"]), "min": _round(agg["score_min"])},
            "hrv_rmssd": {"avg": _round(agg["hrv_avg"])},
            "pvc_per_min_max": None if agg["pvc_max"] is None else int(agg["pvc_max"]),
        },
    }
    return snapshot, row["ts"], dev


SYSTEM_PROMPT = """\
你是一台家用健康监测终端的报告助手，为普通家庭用户解读设备采集到的生理数据。

硬性规则（违反任何一条都算失败）：
1. 你不是医生。禁止下诊断、禁止推断具体疾病名称、禁止预测病情走向。
2. 只能使用 <health_data> 里给出的数值。任何 measured 为 false 的指标，一律说明"本次未采集"，
   绝对不要猜测、推算或编造它的数值。
3. 标了 experimental 的指标（呼吸率、血糖）精度未经临床验证，只能作为参考。这类指标即使偏离
   参考范围，也只能建议"复测"或"咨询医生"，不得据此得出任何健康结论。
4. 心律部分只使用数据里给出的描述性说法（正常搏动 / 室上性异位搏动 / 室性异位搏动 / 融合搏动）
   和 N/S/V/F 字母，不要翻译成具体病名（如"房颤""室速"）。
5. health_score 是设备上的规则式评分，不是临床量表，解读时要说明这一点。
6. 若 data_age_seconds 大于 3600，必须在【提示】里说明这份数据不是刚刚采集的。
7. 用简体中文，语气平实，面向没有医学背景的家庭用户。总长度不超过 400 字。

严格按以下结构输出纯文本（不要 markdown 代码块、不要加粗符号）：
【总体印象】
一到两句话概括本次数据。
【逐项解读】
每个指标一行，格式：指标名 — 数值或"未采集" — 一句话解读。
【建议】
2-3 条具体、可执行的建议。
【提示】
一句免责声明，说明本报告由 AI 生成、仅供参考、不能替代医疗诊断。"""


def _call_deepseek(snapshot):
    user_content = (
        f"以下是设备最近一次上传的数据，以及近 {AI_TREND_HOURS} 小时的统计。"
        f"请严格按系统消息里的规则生成健康报告。\n\n"
        f"<health_data>\n{json.dumps(snapshot, ensure_ascii=False, indent=2)}\n</health_data>"
    )
    resp = requests.post(
        f"{DEEPSEEK_BASE_URL}/chat/completions",
        headers={
            "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
            "Content-Type": "application/json",
        },
        json={
            "model": DEEPSEEK_MODEL,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_content},
            ],
            "temperature": 0.3,
            "max_tokens": 900,
            "stream": False,
        },
        timeout=AI_TIMEOUT,
    )
    if resp.status_code != 200:
        raise UpstreamError(resp.status_code, resp.text[:300])

    try:
        return resp.json()["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, ValueError) as e:
        raise UpstreamError(resp.status_code, f"响应结构异常: {e}") from e


# ------------------------------------------------------------------ #
# 设备屏极简建议：精简 prompt + 后台定期 publish 到设备 advice topic
# ------------------------------------------------------------------ #

SYSTEM_PROMPT_DEVICE = """\
你是一台家用健康监测终端的助手，为设备小屏幕生成极简健康提示。
硬性规则：
1. 你不是医生，禁止下诊断、禁止推断疾病名称。
2. 只用 <health_data> 里 measured 为 true 的数值；未采集的指标不要提。
3. 标了 experimental 的指标（呼吸率/血糖）精度未验证，只能说"参考"或建议复测，不下结论。
4. 心律只用描述性说法（正常/室上性/室性/融合搏动），不要翻译成具体病名。
输出要求：简体中文纯文本，总长不超过 150 字，正好 3 条要点，每条以"· "开头并换行，
面向没有医学背景的家庭用户，语气平实。不要标题、不要 markdown、不要免责声明。"""


def _call_deepseek_device(snapshot):
    """精简 prompt 生成设备屏建议（≤150 字/3 要点），复用 DeepSeek HTTP 逻辑。"""
    user_content = (
        "设备最近数据如下，请按规则生成不超过 150 字的极简健康提示（3 条要点）。\n\n"
        f"<health_data>\n{json.dumps(snapshot, ensure_ascii=False)}\n</health_data>"
    )
    resp = requests.post(
        f"{DEEPSEEK_BASE_URL}/chat/completions",
        headers={
            "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
            "Content-Type": "application/json",
        },
        json={
            "model": DEEPSEEK_MODEL,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT_DEVICE},
                {"role": "user", "content": user_content},
            ],
            "temperature": 0.3,
            "max_tokens": 300,
            "stream": False,
        },
        timeout=AI_TIMEOUT,
    )
    if resp.status_code != 200:
        raise UpstreamError(resp.status_code, resp.text[:300])
    try:
        return resp.json()["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, ValueError) as e:
        raise UpstreamError(resp.status_code, f"响应结构异常: {e}") from e


_device_advice_inflight = threading.Lock()


def _gen_and_publish_device_advice(reason, skip_if_stale=False):
    """生成设备屏建议并 publish。loop(被动兜底) 与 advice_req(按键即时) 共用。

    in-flight 锁：LLM 一次要 5-15s，期间重复触发（狂按按键）直接跳过，
    不排队 —— 排队生成 N 条几乎相同的建议毫无意义还烧额度。
    """
    client = _mqtt_client
    if client is None or not DEEPSEEK_API_KEY:
        return
    if not _device_advice_inflight.acquire(blocking=False):
        print(f"[server] device advice busy, skip ({reason})")
        return
    try:
        snapshot, data_ts, dev = _build_snapshot()
        if snapshot is None:
            print(f"[server] device advice skip: no data yet ({reason})")
            return
        if skip_if_stale and data_ts == _device_advice_state["data_ts"]:
            return   # 被动 loop：无新数据不重复调 LLM；按键请求不受此限

        try:
            advice = _call_deepseek_device(snapshot)
        except Exception as e:   # 网络/超时/上游错误都不该弄崩线程
            print(f"[server] device advice gen failed ({reason}): {e}")
            return

        _device_advice_state["data_ts"] = data_ts
        topic = f"socc/health/{dev}/advice"
        try:
            payload = advice.encode("utf-8")
            client.publish(topic, payload, qos=0)
            print(f"[server] published device advice -> {topic} "
                  f"({len(payload)} bytes, {reason})")
        except Exception as e:
            print(f"[server] publish advice failed: {e}")
    finally:
        _device_advice_inflight.release()


def device_advice_loop():
    """后台线程：定期兜底生成（设备按键请求走 on_message 的即时路径）。"""
    while True:
        time.sleep(DEVICE_ADVICE_INTERVAL_S)
        _gen_and_publish_device_advice("loop", skip_if_stale=True)


@app.route("/api/ai_report", methods=["POST"])
def api_ai_report():
    if not DEEPSEEK_API_KEY:
        return jsonify({
            "error": "服务端未配置 DEEPSEEK_API_KEY，AI 报告不可用。"
                     "在启动 server.py 前设置该环境变量即可开启。"
        }), 503

    body = request.get_json(silent=True) or {}
    force = bool(body.get("force"))

    snapshot, data_ts, dev = _build_snapshot(request.args.get("device_id"))
    if snapshot is None:
        return jsonify({"error": "还没有收到任何设备数据，无法生成报告。"}), 409

    with _ai_lock:
        cached_ok = (
            not force
            and _ai_state["report"] is not None
            and _ai_state["data_ts"] == data_ts
            and _ai_state["device_id"] == dev
        )
        if cached_ok:
            return jsonify({
                "report": _ai_state["report"],
                "model": _ai_state["model"],
                "generated_at": _ai_state["generated_at"],
                "cached": True,
                "data_ts": data_ts,
                "device_id": dev,
                "window_hours": AI_TREND_HOURS,
            })

        waited = time.time() - _ai_state["last_call"]
        if waited < AI_MIN_INTERVAL_S:
            retry_after = int(AI_MIN_INTERVAL_S - waited) + 1
            return jsonify({
                "error": f"请求过于频繁，请 {retry_after} 秒后再试。",
                "retry_after": retry_after,
            }), 429
        _ai_state["last_call"] = time.time()   # 先占位，锁外再真正调用

    try:
        report = _call_deepseek(snapshot)
    except requests.exceptions.Timeout:
        return jsonify({"error": "调用 DeepSeek 超时，请稍后重试。"}), 504
    except UpstreamError as e:
        print(f"[server] deepseek upstream error: {e}")
        return jsonify({"error": f"DeepSeek 接口返回错误（{e.status}），请检查 API key 或额度。"}), 502
    except requests.exceptions.RequestException as e:
        print(f"[server] deepseek request failed: {e}")
        return jsonify({"error": "无法连接 DeepSeek 接口，请检查服务器网络。"}), 502

    generated_at = time.time()
    with _ai_lock:
        _ai_state.update({
            "data_ts": data_ts,
            "device_id": dev,
            "report": report,
            "generated_at": generated_at,
            "model": DEEPSEEK_MODEL,
        })

    print(f"[server] ai report generated for {dev} (data_ts={data_ts})")
    return jsonify({
        "report": report,
        "model": DEEPSEEK_MODEL,
        "generated_at": generated_at,
        "cached": False,
        "data_ts": data_ts,
        "device_id": dev,
        "window_hours": AI_TREND_HOURS,
    })


@app.route("/api/ai_status")
def api_ai_status():
    """前端用它决定「生成报告」按钮是否可点，避免用户点了才发现没配 key。"""
    return jsonify({"enabled": bool(DEEPSEEK_API_KEY), "model": DEEPSEEK_MODEL})


@app.route("/api/push_advice", methods=["GET", "POST"])
def api_push_advice():
    """手动立即生成精简建议并 publish 到设备 advice topic（测试/演示用，不等后台 3 分钟）。

    浏览器直接访问 http://<server>:5000/api/push_advice 即可触发；返回里带 topic 和
    生成的 advice，方便核对 device_id 是否和设备订阅的一致。
    """
    if not DEEPSEEK_API_KEY:
        return jsonify({"error": "未配置 DEEPSEEK_API_KEY"}), 503
    client = _mqtt_client
    if client is None:
        return jsonify({"error": "MQTT 客户端未就绪"}), 503

    snapshot, data_ts, dev = _build_snapshot(request.args.get("device_id"))
    if snapshot is None:
        return jsonify({"error": "还没有收到任何设备数据，无法生成"}), 409

    try:
        advice = _call_deepseek_device(snapshot)
    except Exception as e:
        return jsonify({"error": f"LLM 调用失败: {e}"}), 502

    topic = f"socc/health/{dev}/advice"
    payload = advice.encode("utf-8")
    client.publish(topic, payload, qos=0)
    _device_advice_state["data_ts"] = data_ts
    print(f"[server] push_advice -> {topic} ({len(payload)} bytes)")
    return jsonify({"topic": topic, "device_id": dev, "bytes": len(payload), "advice": advice})


def main():
    if not DEEPSEEK_API_KEY:
        print("[server] DEEPSEEK_API_KEY 未设置，AI 健康报告功能关闭（其余功能正常）")
    t = threading.Thread(target=mqtt_thread_entry, daemon=True)
    t.start()
    t_adv = threading.Thread(target=device_advice_loop, daemon=True)
    t_adv.start()
    app.run(host="0.0.0.0", port=5000, debug=False)


if __name__ == "__main__":
    main()
