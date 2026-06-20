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


CLAUDE_BIN = "/home/pi/.local/bin/claude"


def refresh_token(oauth):
    # Our own curl refresh ALWAYS 429s (request differs from the official
    # client somehow). The official CLI refreshes reliably, so let it do it:
    # `claude -p ok` forces a refresh of ~/.claude/.credentials.json.
    # Driven by the 60s sync loop, this self-heals with no multi-hour gap
    # (unlike the old standalone 7h timer, which left the token expired if a
    # single run failed).
    log("refreshing token via claude CLI...")
    subprocess.run([CLAUDE_BIN, "-p", "ok"], capture_output=True, timeout=90)
    fresh = load_creds()  # re-read the token the CLI just wrote
    if time.time() * 1000 > fresh.get("expiresAt", 0):
        raise RuntimeError("CLI refresh did not produce a valid token")
    log("token refreshed OK (via CLI)")
    return fresh


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
    # refresh 15 min before expiry (margin so we never serve an expired token)
    if time.time() * 1000 > oauth.get("expiresAt", 0) - 900000:
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


MAX_BACKOFF = 600  # 10 min ceiling — CLI refresh self-heals, no need to sleep for hours

if __name__ == "__main__":
    oauth = load_creds()
    backoff = INTERVAL
    while True:
        try:
            oauth = sync_once(oauth)
            backoff = INTERVAL  # success → resume normal cadence
            time.sleep(INTERVAL)
        except HttpError as e:
            # Back off hard on rate limits so we never hammer the token
            # endpoint into a sustained 429 (the failure that froze the badge).
            log(f"sync error: {e}; backing off {backoff}s")
            time.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF)
            try:
                oauth = load_creds()
            except Exception:
                pass
        except Exception as e:
            log(f"sync error: {e}; backing off {backoff}s")
            time.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF)
