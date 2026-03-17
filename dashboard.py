#!/usr/bin/env python3
"""
pi5_dashboard.py
Simple dashboard:
  - Shows live weight from ESP32 (scale/shoot)
  - Shows live pill count from pill counter (scale/info)
  - Name input + Save button → writes row to Google Sheet
"""

import json
import time
import threading
import logging

import paho.mqtt.client as mqtt
import gspread

from flask import Flask, render_template_string, request, jsonify
from flask_socketio import SocketIO

# =====================================================================
# CONFIGURATION
# =====================================================================
MQTT_BROKER      = "localhost"
MQTT_PORT        = 1883
SHEET_NAME       = "Medicine_data"
CREDENTIALS_FILE = "credentials.json"
T_SHOOT          = "scale/shoot"   # ESP32 → weight
T_INFO           = "scale/info"    # pill counter → pill count

# =====================================================================
# FLASK / SOCKETIO
# =====================================================================
app      = Flask(__name__)
app.config["SECRET_KEY"] = "dashboard_secret"
socketio = SocketIO(app, async_mode="threading", cors_allowed_origins="*")

# =====================================================================
# LOGGING
# =====================================================================
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(levelname)s] %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger(__name__)

# =====================================================================
# SHARED STATE
# =====================================================================
state_lock = threading.Lock()
state = {"weight": 0.0, "pill_count": 0}

# =====================================================================
# GOOGLE SHEETS
# =====================================================================
_sheet = None

def open_sheet():
    global _sheet
    client = gspread.service_account(filename=CREDENTIALS_FILE)
    _sheet = client.open(SHEET_NAME).sheet1
    if not _sheet.row_values(1):
        _sheet.append_row(["Timestamp", "Name", "Pills_Number"])
    log.info("Sheet '%s' ready.", SHEET_NAME)

# =====================================================================
# MQTT
# =====================================================================
def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.value == 0:
        log.info("MQTT connected.")
        client.subscribe(T_SHOOT)
        client.subscribe(T_INFO)
    else:
        log.error("MQTT connect failed rc=%d", reason_code.value)

def on_disconnect(client, userdata, flags, reason_code, properties):
    log.warning("MQTT disconnected. Auto-reconnect...")

def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode(errors="replace"))
    except Exception:
        return

    if msg.topic == T_SHOOT:
        weight = float(data.get("weight_kg", data.get("weight", 0.0)))
        with state_lock:
            state["weight"] = round(weight, 2)
        socketio.emit("update", {"weight": state["weight"]})
        log.info("Weight: %.2f kg", weight)

    elif msg.topic == T_INFO:
        pill_count = int(data.get("pill_count", 0))
        with state_lock:
            state["pill_count"] = pill_count
        socketio.emit("update", {"pill_count": pill_count})
        log.info("Pill count: %d", pill_count)

def start_mqtt():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="Pi5_Dashboard")
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message
    client.reconnect_delay_set(1, 30)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_forever()

# =====================================================================
# FLASK ROUTES
# =====================================================================
HTML = """
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Medicine Dashboard</title>
  <style>
    body { font-family: Arial, sans-serif; background: #111; color: #eee;
           display: flex; flex-direction: column; align-items: center;
           padding: 40px; gap: 30px; }
    .card { background: #222; border-radius: 16px; padding: 30px 50px;
            text-align: center; min-width: 220px; }
    .label { font-size: 14px; color: #aaa; margin-bottom: 8px; }
    .value { font-size: 56px; font-weight: bold; color: #0f0; }
    .row   { display: flex; gap: 30px; flex-wrap: wrap; justify-content: center; }
    input  { font-size: 20px; padding: 10px 16px; border-radius: 8px;
             border: 1px solid #555; background: #333; color: #eee;
             width: 260px; }
    button { font-size: 20px; padding: 10px 30px; border-radius: 8px;
             border: none; background: #0a0; color: #fff; cursor: pointer; }
    button:hover { background: #0c0; }
    #msg   { font-size: 16px; color: #8f8; min-height: 24px; }
  </style>
</head>
<body>
  <h2>Medicine Dashboard</h2>
  <div class="row">
    <div class="card">
      <div class="label">Weight (kg)</div>
      <div class="value" id="weight">{{ weight }}</div>
    </div>
    <div class="card">
      <div class="label">Pill Count</div>
      <div class="value" id="pill_count">{{ pill_count }}</div>
    </div>
  </div>

  <div class="card" style="display:flex; flex-direction:column; gap:14px; align-items:center;">
    <div class="label">Medicine Name</div>
    <input id="name" type="text" placeholder="e.g. Paracetamol">
    <button onclick="save()">Save to Sheet</button>
    <div id="msg"></div>
  </div>

  <script src="https://cdn.socket.io/4.6.1/socket.io.min.js"></script>
  <script>
    const socket = io();
    socket.on("update", d => {
      if (d.weight     !== undefined) document.getElementById("weight").textContent     = d.weight;
      if (d.pill_count !== undefined) document.getElementById("pill_count").textContent = d.pill_count;
    });

    function save() {
      const name = document.getElementById("name").value.trim();
      if (!name) { document.getElementById("msg").textContent = "Enter a name first."; return; }
      fetch("/api/save", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({name})
      })
      .then(r => r.json())
      .then(d => {
        document.getElementById("msg").textContent = d.ok ? "Saved! Row " + d.row : "Error: " + d.error;
      });
    }
  </script>
</body>
</html>
"""

@app.route("/")
def index():
    with state_lock:
        snap = dict(state)
    return render_template_string(HTML, **snap)

@app.route("/api/save", methods=["POST"])
def api_save():
    body = request.get_json(silent=True) or {}
    name = str(body.get("name", "")).strip()[:80]
    if not name:
        return jsonify({"ok": False, "error": "Name is empty"}), 400
    if _sheet is None:
        return jsonify({"ok": False, "error": "Sheet not connected"}), 500

    with state_lock:
        weight     = state["weight"]
        pill_count = state["pill_count"]

    try:
        ts  = time.strftime("%Y-%m-%d %H:%M:%S")
        _sheet.append_row([ts, name, pill_count])
        row = len(_sheet.get_all_values())
        log.info("Saved: %s | %d pills | row %d", name, pill_count, row)
        return jsonify({"ok": True, "row": row})
    except Exception as exc:
        log.error("Sheet write error: %s", exc)
        return jsonify({"ok": False, "error": str(exc)}), 500

# =====================================================================
# MAIN
# =====================================================================
if __name__ == "__main__":
    threading.Thread(target=lambda: open_sheet() if True else None,
                     daemon=True).start()
    threading.Thread(target=start_mqtt, daemon=True).start()

    log.info("Dashboard at http://0.0.0.0:5000")
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)
