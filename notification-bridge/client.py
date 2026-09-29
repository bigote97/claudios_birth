"""Cliente del bridge. Los hooks y la CLI lo usan para no bloquear al agente."""

import json
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BRIDGE_URL = "http://127.0.0.1:8765"
CONFIG_PATH = ROOT / "config.json"

DEFAULT_MESSAGES = {
    ("cursor", "approval"): "Cursor necesita aprobacion",
    ("cursor", "finished"): "Cursor termino su tarea",
    ("cursor", "error"): "Cursor tuvo un problema",
    ("cursor", "slow"): "Cursor esta tardando bastante",
    ("claude", "approval"): "Claude necesita aprobacion",
    ("claude", "finished"): "Claude termino su tarea",
    ("claude", "error"): "Claude tuvo un problema",
    ("claude", "slow"): "Claude esta tardando bastante",
}


def load_config():
    if not CONFIG_PATH.exists():
        return {
            "esp_host": "",
            "esp_host_lock": False,
            "owner": "",
            "device_name": "",
            "device_owner": "",
            "device_mac": "",
            "cursor_approval": True,
            "approval_cooldown_s": 25,
        }
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def cooldown_ok(slot, seconds):
    path = Path(tempfile.gettempdir()) / f"desk-companion-{slot}.txt"
    now = time.time()
    try:
        last = float(path.read_text(encoding="utf-8").strip() or "0")
    except (OSError, ValueError):
        last = 0
    if now - last < seconds:
        return False
    try:
        path.write_text(str(now), encoding="utf-8")
    except OSError:
        pass
    return True


def bridge_is_up():
    try:
        with urllib.request.urlopen(BRIDGE_URL + "/health", timeout=0.4) as resp:
            return resp.status == 200
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def ensure_bridge():
    if bridge_is_up():
        return True
    kwargs = {
        "cwd": str(ROOT),
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "stdin": subprocess.DEVNULL,
    }
    if sys.platform == "win32":
        kwargs["creationflags"] = (
            subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        )
    try:
        subprocess.Popen([sys.executable, str(ROOT / "bridge.py"), "--daemon"], **kwargs)
    except OSError as exc:
        print(f"no pude arrancar el bridge: {exc}", file=sys.stderr)
        return False
    for _ in range(10):
        time.sleep(0.15)
        if bridge_is_up():
            return True
    return False


def post_notify(source, event_type, message=None):
    if message is None:
        message = DEFAULT_MESSAGES.get((source, event_type), "")
    payload = {"source": source, "type": event_type}
    if message:
        payload["message"] = message
    if not ensure_bridge():
        return False
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BRIDGE_URL + "/notify",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=1.2) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"bridge no acepto el evento: {exc}", file=sys.stderr)
        return False
