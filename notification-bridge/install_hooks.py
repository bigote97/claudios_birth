"""Instala los hooks de usuario de Cursor y Claude Code sin pisar otros permisos."""

import json
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PYTHON = Path(sys.executable)
CURSOR_HOOKS = Path.home() / ".cursor" / "hooks.json"
CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"

def quote_cmd(script, arg):
    py = str(PYTHON)
    sc = str(ROOT / script)
    if sys.platform == "win32":
        return f'"{py}" "{sc}" {arg}'
    return f"{shlex.quote(py)} {shlex.quote(sc)} {arg}"


CURSOR_CMD = {
    "working": quote_cmd("hook_cursor.py", "working"),
    "stop": quote_cmd("hook_cursor.py", "stop"),
    "approval": quote_cmd("hook_cursor.py", "approval"),
}
CLAUDE_CMD = {
    "working": quote_cmd("hook_claude.py", "working"),
    "stop": quote_cmd("hook_claude.py", "stop"),
    "approval": quote_cmd("hook_claude.py", "approval"),
}


def load(path):
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def upsert_cursor(hooks, event, command):
    action = command.strip().rstrip('"').split()[-1]
    kept = False
    fresh = []
    for entry in hooks.get(event, []):
        cmd = entry.get("command", "")
        if "hook_cursor.py" in cmd and cmd.strip().rstrip('"').endswith(action):
            if kept:
                continue
            entry["command"] = command
            entry["timeout"] = 8
            kept = True
        fresh.append(entry)
    if not kept:
        fresh.append({"command": command, "timeout": 8})
    hooks[event] = fresh


def upsert_claude(hooks, event, command):
    groups = hooks.setdefault(event, [])
    for group in groups:
        for hook in group.get("hooks", []):
            if "hook_claude.py" in hook.get("command", ""):
                hook["command"] = command
                hook["type"] = "command"
                hook["timeout"] = 10
                hook["async"] = True
                return
    groups.append({
        "hooks": [{
            "type": "command",
            "command": command,
            "timeout": 10,
            "async": True,
        }]
    })


def main():
    if not PYTHON.exists():
        raise SystemExit(f"No encuentro Python en {PYTHON}")

    cursor = load(CURSOR_HOOKS)
    cursor["version"] = 1
    cursor_hooks = cursor.setdefault("hooks", {})
    upsert_cursor(cursor_hooks, "beforeSubmitPrompt", CURSOR_CMD["working"])
    upsert_cursor(cursor_hooks, "stop", CURSOR_CMD["stop"])
    upsert_cursor(cursor_hooks, "beforeShellExecution", CURSOR_CMD["approval"])
    upsert_cursor(cursor_hooks, "beforeMCPExecution", CURSOR_CMD["approval"])
    save(CURSOR_HOOKS, cursor)

    original = load(CLAUDE_SETTINGS)
    allow_before = list(original.get("permissions", {}).get("allow", []))
    claude_hooks = original.setdefault("hooks", {})
    upsert_claude(claude_hooks, "UserPromptSubmit", CLAUDE_CMD["working"])
    upsert_claude(claude_hooks, "PermissionRequest", CLAUDE_CMD["approval"])
    upsert_claude(claude_hooks, "Notification", CLAUDE_CMD["approval"])
    upsert_claude(claude_hooks, "Stop", CLAUDE_CMD["stop"])
    save(CLAUDE_SETTINGS, original)

    check = load(CLAUDE_SETTINGS)
    allow_after = list(check.get("permissions", {}).get("allow", []))
    if allow_before != allow_after:
        raise SystemExit("El instalador cambio permissions.allow; no deberia pasar")
    print(f"Cursor hooks: {CURSOR_HOOKS}")
    print(f"Claude hooks: {CLAUDE_SETTINGS}")


if __name__ == "__main__":
    main()
