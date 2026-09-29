"""
Bridge local de Claudio's Birth.

Escucha en 127.0.0.1:8765. Cuando el ESP32 entra al Wi-Fi anuncia por UDP
(puerto 8766) "Claudio's Birth de {dueno}". Este proceso lo reconoce y
reenvia POST /notify. Si el dispositivo todavia no aparece, guarda la cola.

  python bridge.py
  python bridge.py --daemon
  python notify.py cursor approval
"""

import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request

from client import CONFIG_PATH, load_config

HOST = "127.0.0.1"
PORT = 8765
BEACON_PORT = 8766
PRODUCT_ID = "claudios-birth"
LEGACY_ID = "desk-companion"
STALE_S = 35
ROOT_LOG = CONFIG_PATH.parent / "bridge.log"
PID_PATH = CONFIG_PATH.parent / "bridge.pid"

lock = threading.Lock()
device = {"ip": "", "owner": "", "name": "", "mac": "", "seen": 0.0}
pending = []
last_error = ""


def log(msg):
    print(msg, flush=True)


def parse_beacon(text, addr):
    text = (text or "").strip()
    fallback_ip = addr[0] if addr else ""
    if text.startswith("{"):
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return None
        ident = str(data.get("id") or "")
        if ident not in (PRODUCT_ID, LEGACY_ID):
            return None
        ip = str(data.get("ip") or fallback_ip).strip()
        owner = str(data.get("owner") or "").strip()
        name = str(data.get("name") or "").strip()
        mac = str(data.get("mac") or "").strip().upper()
        if not name:
            name = f"Claudio's Birth de {owner}" if owner else "Claudio's Birth"
        if not ip:
            return None
        return {"ip": ip, "owner": owner, "name": name, "mac": mac}
    parts = text.split()
    if not parts or parts[0] not in (PRODUCT_ID, LEGACY_ID):
        return None
    ip = parts[1] if len(parts) > 1 else fallback_ip
    if not ip:
        return None
    return {"ip": ip, "owner": "", "name": "Claudio's Birth", "mac": ""}


def persist_device():
    with lock:
        ip = device["ip"]
        name = device["name"]
        owner = device["owner"]
        mac = device["mac"]
    cfg = load_config()
    if cfg.get("esp_host_lock") and cfg.get("esp_host"):
        return
    cfg["esp_host"] = ip
    cfg["device_name"] = name
    cfg["device_owner"] = owner
    cfg["device_mac"] = mac
    cfg.setdefault("esp_host_lock", False)
    cfg.setdefault("owner", "")
    cfg.setdefault("cursor_approval", True)
    cfg.setdefault("approval_cooldown_s", 25)
    try:
        CONFIG_PATH.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    except OSError as exc:
        log(f"no pude guardar config.json: {exc}")


def consider(found, source):
    cfg = load_config()
    wanted = (cfg.get("owner") or "").strip().lower()
    found_owner = (found.get("owner") or "").strip()
    if wanted and found_owner and wanted != found_owner.lower():
        return False
    if cfg.get("esp_host_lock") and cfg.get("esp_host") and found["ip"] != cfg.get("esp_host"):
        return False

    now = time.time()
    with lock:
        age = now - device["seen"] if device["seen"] else 1e9
        fresh = bool(device["ip"]) and age < STALE_S
        same_ip = device["ip"] == found["ip"]
        same_mac = bool(device["mac"] and found.get("mac") and device["mac"] == found["mac"])
        if fresh and not same_ip and not same_mac:
            current_owner = (device["owner"] or "").lower()
            new_owner = found_owner.lower()
            if current_owner and new_owner and current_owner != new_owner:
                return False
            if not current_owner and not new_owner:
                return False
        name = found.get("name") or device["name"] or "Claudio's Birth"
        owner = found_owner or device["owner"]
        mac = found.get("mac") or device["mac"]
        changed = device["ip"] != found["ip"] or device["name"] != name or device["owner"] != owner
        device["ip"] = found["ip"]
        device["owner"] = owner
        device["name"] = name
        device["mac"] = mac
        device["seen"] = now
        snapshot = dict(device)
    if changed:
        log(f"reconocido: {snapshot['name']} en {snapshot['ip']} ({source})")
        persist_device()
    return True


def current_esp():
    with lock:
        return device["ip"]


def remember(payload):
    with lock:
        pending.append(payload)
        if len(pending) > 32:
            del pending[0]


def take_pending():
    with lock:
        items = list(pending)
        pending.clear()
        return items


def restore(items):
    with lock:
        pending[:0] = items
        if len(pending) > 32:
            del pending[32:]


def forward(payload, host):
    global last_error
    data = json.dumps(payload).encode("utf-8")
    req = request.Request(
        f"http://{host}/notify",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with request.urlopen(req, timeout=2.0) as resp:
            resp.read()
            return True
    except (error.URLError, TimeoutError, OSError) as exc:
        last_error = str(exc)
        return False


def flush_pending():
    host = current_esp()
    if not host:
        return
    items = take_pending()
    for index, item in enumerate(items):
        if not forward(item, host):
            restore(items[index:])
            return


def beacon_loop():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    except OSError:
        pass
    try:
        sock.bind(("0.0.0.0", BEACON_PORT))
    except OSError as exc:
        log(f"UDP {BEACON_PORT} no disponible ({exc}). Sigo con el ultimo dispositivo conocido.")
        return
    log(f"escuchando beacons UDP :{BEACON_PORT}")
    while True:
        try:
            data, addr = sock.recvfrom(512)
        except OSError:
            time.sleep(1)
            continue
        found = parse_beacon(data.decode("utf-8", errors="ignore"), addr)
        if found:
            consider(found, "beacon")


def retry_loop():
    while True:
        time.sleep(2)
        flush_pending()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path != "/health":
            self._send(404, {"ok": False})
            return
        now = time.time()
        with lock:
            queued = len(pending)
            ip = device["ip"]
            name = device["name"]
            owner = device["owner"]
            seen = round(now - device["seen"], 1) if device["seen"] else None
            err = last_error
        self._send(200, {
            "ok": True,
            "esp_host": ip,
            "device_name": name,
            "owner": owner,
            "seen_s": seen,
            "queued": queued,
            "last_error": err,
        })

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path != "/notify":
            self._send(404, {"ok": False})
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8") or "{}")
        except json.JSONDecodeError:
            self._send(400, {"ok": False, "error": "json"})
            return
        if not payload.get("type"):
            self._send(400, {"ok": False, "error": "type"})
            return
        payload.setdefault("source", "cursor")
        host = current_esp()
        if host and forward(payload, host):
            self._send(200, {"ok": True, "delivered": True, "esp_host": host})
            return
        remember(payload)
        self._send(202, {"ok": True, "delivered": False, "queued": True})

    def log_message(self, fmt, *args):
        log("[http] " + (fmt % args))


def enable_daemon_log():
    try:
        if ROOT_LOG.exists() and ROOT_LOG.stat().st_size > 1_000_000:
            ROOT_LOG.write_text("", encoding="utf-8")
        handle = open(ROOT_LOG, "a", encoding="utf-8", buffering=1)
    except OSError as exc:
        log(f"no pude abrir el log: {exc}")
        return
    sys.stdout = handle
    sys.stderr = handle


def load_cached():
    cfg = load_config()
    host = (cfg.get("esp_host") or "").strip()
    if not host:
        log("esperando a Claudio's Birth en la red Wi-Fi")
        return
    consider({
        "ip": host,
        "owner": cfg.get("device_owner") or "",
        "name": cfg.get("device_name") or "Claudio's Birth",
        "mac": (cfg.get("device_mac") or "").upper(),
    }, "cache")


def main():
    if "--daemon" in sys.argv:
        enable_daemon_log()
    try:
        PID_PATH.write_text(str(os.getpid()), encoding="utf-8")
    except OSError:
        pass
    load_cached()
    threading.Thread(target=beacon_loop, daemon=True).start()
    threading.Thread(target=retry_loop, daemon=True).start()
    try:
        httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    except OSError as exc:
        log(f"puerto {PORT} ocupado ({exc}). El bridge ya esta corriendo.")
        return 0
    log(f"notification bridge en http://{HOST}:{PORT}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("bridge detenido")
    finally:
        httpd.server_close()
        try:
            PID_PATH.unlink(missing_ok=True)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
