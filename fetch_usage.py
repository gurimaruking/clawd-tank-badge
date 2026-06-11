"""Fetch Claude usage from claude.ai API using Chrome session cookie."""
import json, os, sys, sqlite3, base64, shutil, tempfile, time, struct
from pathlib import Path
from urllib.request import Request, urlopen

def get_chrome_key():
    local_state = Path(os.environ["LOCALAPPDATA"]) / "Google/Chrome/User Data/Local State"
    with open(local_state, "r", encoding="utf-8") as f:
        data = json.load(f)
    encrypted_key = base64.b64decode(data["os_crypt"]["encrypted_key"])
    encrypted_key = encrypted_key[5:]  # strip "DPAPI" prefix
    import ctypes, ctypes.wintypes
    class DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", ctypes.wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]
    blob_in = DATA_BLOB(len(encrypted_key), ctypes.create_string_buffer(encrypted_key, len(encrypted_key)))
    blob_out = DATA_BLOB()
    if not ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out)):
        raise RuntimeError("DPAPI decryption failed")
    key = ctypes.string_at(blob_out.pbData, blob_out.cbData)
    ctypes.windll.kernel32.LocalFree(blob_out.pbData)
    return key

def decrypt_cookie(encrypted_value, key):
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    encrypted_value = encrypted_value[3:]  # strip "v10" prefix
    nonce = encrypted_value[:12]
    ciphertext = encrypted_value[12:]
    return AESGCM(key).decrypt(nonce, ciphertext, None).decode("utf-8")

def get_session_cookie():
    key = get_chrome_key()
    cookie_path = Path(os.environ["LOCALAPPDATA"]) / "Google/Chrome/User Data/Default/Network/Cookies"
    tmp = tempfile.mktemp(suffix=".db")
    # Chrome locks the file; use Win32 API with shared read access
    import ctypes, ctypes.wintypes
    GENERIC_READ = 0x80000000
    FILE_SHARE_READ = 1
    FILE_SHARE_WRITE = 2
    FILE_SHARE_DELETE = 4
    OPEN_EXISTING = 3
    handle = ctypes.windll.kernel32.CreateFileW(
        str(cookie_path), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        None, OPEN_EXISTING, 0, None)
    if handle == -1:
        raise RuntimeError("Cannot open Chrome cookies file")
    size = ctypes.windll.kernel32.GetFileSize(handle, None)
    buf = ctypes.create_string_buffer(size)
    read = ctypes.wintypes.DWORD()
    ctypes.windll.kernel32.ReadFile(handle, buf, size, ctypes.byref(read), None)
    ctypes.windll.kernel32.CloseHandle(handle)
    with open(tmp, "wb") as dst:
        dst.write(buf.raw)
    try:
        conn = sqlite3.connect(tmp)
        row = conn.execute(
            "SELECT encrypted_value FROM cookies WHERE host_key='.claude.ai' AND name='sessionKey'",
        ).fetchone()
        conn.close()
        if not row:
            raise RuntimeError("sessionKey cookie not found")
        return decrypt_cookie(row[0], key)
    finally:
        os.unlink(tmp)

def fetch_usage(session_key):
    org_url = "https://claude.ai/api/organizations"
    req = Request(org_url, headers={"Cookie": f"sessionKey={session_key}"})
    with urlopen(req, timeout=5) as resp:
        orgs = json.loads(resp.read())
    org_uuid = None
    for org in orgs:
        if "chat" in org.get("capabilities", []):
            org_uuid = org["uuid"]
            break
    if not org_uuid:
        raise RuntimeError("No chat org found")
    usage_url = f"https://claude.ai/api/organizations/{org_uuid}/usage"
    req = Request(usage_url, headers={"Cookie": f"sessionKey={session_key}"})
    with urlopen(req, timeout=5) as resp:
        return json.loads(resp.read())

def main():
    try:
        sk = get_session_cookie()
        usage = fetch_usage(sk)
        five_hour = usage.get("five_hour") or {}
        utilization = five_hour.get("utilization", 0)
        resets_at = five_hour.get("resets_at", "")
        plan_tokens = int(800000 * utilization / 100)
        plan_reset = 0
        if resets_at:
            from datetime import datetime, timezone
            reset_dt = datetime.fromisoformat(resets_at)
            now = datetime.now(timezone.utc)
            plan_reset = max(0, int((reset_dt - now).total_seconds()))
        result = {"plan_tokens": plan_tokens, "plan_reset": plan_reset, "utilization": utilization}
        print(json.dumps(result))
    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
