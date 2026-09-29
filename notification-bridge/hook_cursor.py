"""Hook de Cursor. No decide permisos: solo avisa al companion y sale 0."""

import json
import sys

from client import cooldown_ok, load_config, post_notify


def read_stdin():
    try:
        raw = sys.stdin.read()
    except Exception:
        return {}
    if not raw or not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {}


def main():
    event = sys.argv[1] if len(sys.argv) > 1 else ""
    data = read_stdin()
    try:
        dispatch(event, data)
    except Exception as exc:
        print(f"hook cursor: {exc}", file=sys.stderr)
    if event == "working":
        sys.stdout.write('{"continue": true}')
        sys.stdout.flush()
    return 0


def dispatch(event, data):
    if event == "working":
        post_notify("cursor", "working", "")
        return
    if event == "stop":
        status = data.get("status") or "completed"
        if status == "error":
            post_notify("cursor", "error")
        elif status == "aborted":
            post_notify("cursor", "idle", "")
        else:
            post_notify("cursor", "finished")
        return
    if event == "approval":
        cfg = load_config()
        if not cfg.get("cursor_approval", True):
            return
        seconds = float(cfg.get("approval_cooldown_s", 25))
        if not cooldown_ok("cursor-approval", seconds):
            return
        post_notify("cursor", "approval")


if __name__ == "__main__":
    raise SystemExit(main())
