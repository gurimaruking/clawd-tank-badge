#!/usr/bin/env python3
"""Clawd Usage Sync — polls Anthropic usage API, feeds the local relay.

Runs 24/7 on the Pi. Uses the Pi's OWN Claude Code credentials
(~/.claude/.credentials.json, created once via `claude` login on the Pi).
Auto-refreshes the access token with the stored refresh token, so it
keeps working indefinitely without a PC or browser.

All HTTP goes through curl: Python's TLS fingerprint is blocked by
Cloudflare on Anthropic endpoints.
"""

import json, os, subprocess, time
from datetime import datetime

CRED_FILE = os.path.expanduser("~/.claude/.credentials.json")
TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
RELAY_URL = "http://localhost:8070/event"
CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
INTERVAL = 60  # seconds between syncs


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class HttpError(Exception):
    def __init__(self, code, body):
        self.code = code
        self.body = body
        super().__init__(f"HTTP {code}: {body[:200]}")


def http_json(url, data=None, headers=None):
    cmd = ["curl", "-s", "-w", "\n%{http_code}", "--max-time", "20",
           "-H", "Content-Type: application/json"]
    for k, v in (headers or {}).items():
        cmd += ["-H", f"{k}: {v}"]
    if data is not None:
        cmd += ["-X", "POST", "-d", json.dumps(data)]
    cmd.append(url)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    body, _, code = out.rpartition("\n")
    code = int(code) if code.isdigit() else 0
    if code < 200 or code >= 300:
        raise HttpError(code, body)
    return json.loads(body) if body.strip() else {}


def load_creds():
    with open(CRED_FILE) as f:
        return json.load(f)["claudeAiOauth"]


def save_creds(oauth):
    with open(CRED_FILE) as f:
        data = json.load(f)
    data["claudeAiOauth"] = oauth
    with open(CRED_FILE, "w") as f:
        json.dump(data, f)


def refresh_token(oauth):
    log("refreshing access token...")
    r = http_json(TOKEN_URL, data={
        "grant_type": "refresh_token",
        "refresh_token": oauth["refreshToken"],
        "client_id": CLIENT_ID,
    })
    oauth["accessToken"] = r["access_token"]
    if r.get("refresh_token"):
        oauth["refreshToken"] = r["refresh_token"]
    oauth["expiresAt"] = int((time.time() + r.get("expires_in", 3600)) * 1000)
    save_creds(oauth)
    log("token refreshed OK")
    return oauth


def get_usage(oauth):
    return http_json(USAGE_URL, headers={
        "Authorization": f"Bearer {oauth['accessToken']}",
        "anthropic-beta": "oauth-2025-04-20",
    })


def find_five_hour(usage):
    """Locate the 5h window block; tolerate API shape variations."""
    if isinstance(usage.get("five_hour"), dict):
        return usage["five_hour"]
    for v in usage.values():
        if isinstance(v, dict) and "utilization" in v:
            return v
    return None


def sync_once(oauth):
    # refresh 5 min before expiry
    if time.time() * 1000 > oauth.get("expiresAt", 0) - 300000:
        oauth = refresh_token(oauth)
    try:
        usage = get_usage(oauth)
    except HttpError as e:
        if e.code == 401:
            oauth = refresh_token(oauth)
            usage = get_usage(oauth)
        else:
            raise

    fh = find_five_hour(usage)
    if not fh:
        log(f"unexpected usage shape: {json.dumps(usage)[:300]}")
        return oauth

    util = fh.get("utilization")
    resets_at = 0
    if fh.get("resets_at"):
        resets_at = datetime.fromisoformat(fh["resets_at"]).timestamp()

    payload = {"session": "pi-sync"}
    if util is not None:
        payload["utilization"] = util
    if resets_at:
        payload["reset_at"] = resets_at
    http_json(RELAY_URL, data=payload)

    mins = max(0, int((resets_at - time.time()) / 60)) if resets_at else -1
    log(f"synced: {util}% used, resets in {mins}min")
    return oauth


if __name__ == "__main__":
    oauth = load_creds()
    while True:
        try:
            oauth = sync_once(oauth)
        except Exception as e:
            log(f"sync error: {e}")
            try:
                oauth = load_creds()  # re-read in case claude CLI rotated tokens
            except Exception:
                pass
        time.sleep(INTERVAL)
