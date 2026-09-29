"""Hook de Claude Code. No escribe una decision: el prompt de permiso sigue igual."""

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
        print(f"hook claude: {exc}", file=sys.stderr)
    return 0


def dispatch(event, data):
    if event == "working":
        post_notify("claude", "working", "")
        return
    if event == "stop":
        if data.get("stop_hook_active"):
            return
        post_notify("claude", "finished")
        return
    if event == "approval":
        cfg = load_config()
        seconds = float(cfg.get("approval_cooldown_s", 25))
        if not cooldown_ok("claude-approval", seconds):
            return
        message = data.get("message") or ""
        lowered = message.lower()
        if "waiting" in lowered or "input" in lowered:
            post_notify("claude", "approval", "Claude te esta esperando")
        else:
            post_notify("claude", "approval")


if __name__ == "__main__":
    raise SystemExit(main())
