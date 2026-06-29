#!/usr/bin/env python3
"""Clawd Relay — event broker for ESP32 badge. Runs 24/7 on the Pi.

State model (all absolute values — relaying never introduces drift):
  tool/hook/session : latest Claude Code activity (from PC hooks)
  utilization       : 0-100 plan usage percent (from usage-sync)
  reset_at          : epoch when the 5h window resets (from usage-sync)
  updated_at        : when the last ACTIVITY event arrived

When now >= reset_at the window rolled over: usage data auto-expires.

Endpoints:
  POST /event  — hooks (tool/hook/session) and usage-sync (utilization/reset_at)
  GET  /state  — full state + age (for ESP32 polling)
  GET  /       — health check
"""

import json, os, time, threading
from http.server import HTTPServer, BaseHTTPRequestHandler

STATE_FILE = os.path.expanduser("~/clawd-sync/state.json")

STATE = {
    "tool": "",
    "hook": "",
    "session": "",
    "utilization": -1,   # -1 = unknown
    "reset_at": 0,       # 0 = unknown
    "updated_at": 0,
}
LOCK = threading.Lock()

def load_state():
    try:
        with open(STATE_FILE) as f:
            saved = json.load(f)
        for k in STATE:
            if k in saved:
                STATE[k] = saved[k]
    except (OSError, json.JSONDecodeError):
        pass

def save_state():
    try:
        with open(STATE_FILE, "w") as f:
            json.dump(STATE, f)
    except OSError:
        pass

def expire_if_needed():
    """Window rolled over → clear usage (call with LOCK held)."""
    if STATE["reset_at"] > 0 and time.time() >= STATE["reset_at"]:
        STATE["utilization"] = -1
        STATE["reset_at"] = 0
        save_state()

class RelayHandler(BaseHTTPRequestHandler):
    def _send(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        if isinstance(body, str):
            body = body.encode()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/event":
            self._send(404, "text/plain", "Not Found")
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            data = json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            self._send(400, "text/plain", "Bad JSON")
            return

        with LOCK:
            is_activity = False
            for key in ("tool", "hook", "session"):
                if key in data and data[key]:
                    STATE[key] = data[key]
                    if key != "session":
                        is_activity = True
            if "utilization" in data and data["utilization"] is not None:
                STATE["utilization"] = float(data["utilization"])
            if "reset_at" in data and data["reset_at"]:
                STATE["reset_at"] = float(data["reset_at"])
            # updated_at marks activity freshness only — usage updates
            # shouldn't make the crab look "active"
            if is_activity:
                STATE["updated_at"] = time.time()
            expire_if_needed()
            save_state()

        self._send(200, "application/json", '{"status":"ok"}')

    def do_GET(self):
        if self.path == "/state":
            with LOCK:
                expire_if_needed()
                payload = dict(STATE)
            payload["age"] = int(time.time() - payload["updated_at"]) if payload["updated_at"] else -1
            # remain_sec: countdown computed with the Pi's authoritative clock,
            # so the badge needs no working NTP of its own (-1 = unknown).
            if payload["reset_at"] and payload["reset_at"] > 0:
                payload["remain_sec"] = max(0, int(payload["reset_at"] - time.time()))
            else:
                payload["remain_sec"] = -1
            self._send(200, "application/json", json.dumps(payload))
        elif self.path == "/":
            self._send(200, "text/plain", "Clawd Relay OK")
        else:
            self._send(404, "text/plain", "Not Found")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def log_message(self, format, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {args[0]}")

if __name__ == "__main__":
    load_state()
    server = HTTPServer(("0.0.0.0", 8070), RelayHandler)
    print("Clawd Relay v3 running on :8070")
    server.serve_forever()
