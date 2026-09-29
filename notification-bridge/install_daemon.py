"""Instala el bridge como daemon del usuario en Windows, macOS o Linux.

Arranca al iniciar sesion, se reinicia si se cae, y reconoce al ESP32
en la red Wi-Fi sin una IP fija.

  python install_daemon.py
  python install_daemon.py remove
  python install_daemon.py status
"""

import json
import os
import plistlib
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BRIDGE = ROOT / "bridge.py"
TASK_NAME = "ClaudiosBirth"
MAC_LABEL = "com.claudiosbirth.bridge"
UNIT_NAME = "claudios-birth.service"
HEALTH_URL = "http://127.0.0.1:8765/health"


def python_for_daemon():
    exe = Path(sys.executable)
    if sys.platform == "win32":
        pythonw = exe.with_name("pythonw.exe")
        if pythonw.exists():
            return pythonw
    return exe


def xml_escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def health():
    try:
        with urllib.request.urlopen(HEALTH_URL, timeout=0.8) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except (OSError, json.JSONDecodeError, urllib.error.URLError):
        return None


def command_line(pid):
    try:
        out = subprocess.check_output(
            ["wmic", "process", "where", f"ProcessId={pid}", "get", "CommandLine", "/value"],
            text=True,
            errors="replace",
        )
    except (OSError, subprocess.CalledProcessError):
        return ""
    for line in out.splitlines():
        if line.startswith("CommandLine="):
            return line.split("=", 1)[1].strip()
    return ""


def stop_running_bridge():
    if sys.platform == "win32":
        try:
            netstat = subprocess.check_output(["netstat", "-ano", "-p", "TCP"], text=True, errors="replace")
        except (OSError, subprocess.CalledProcessError):
            netstat = ""
        for line in netstat.splitlines():
            if ":8765" not in line or "LISTENING" not in line:
                continue
            pid = line.split()[-1]
            if "bridge.py" in command_line(pid):
                subprocess.run(["taskkill", "/PID", pid, "/F"], check=False, capture_output=True)
        try:
            listed = subprocess.check_output(["wmic", "process", "get", "ProcessId,CommandLine", "/format:csv"], text=True, errors="replace")
        except (OSError, subprocess.CalledProcessError):
            listed = ""
        for row in listed.splitlines():
            if "ClaudiosBirth.vbs" not in row and "ai-desk-companion" not in row:
                continue
            if "bridge.py" not in row and "ClaudiosBirth.vbs" not in row:
                continue
            cells = row.split(",")
            pid = cells[-1].strip() if cells else ""
            if pid.isdigit():
                subprocess.run(["taskkill", "/PID", pid, "/F"], check=False, capture_output=True)
        subprocess.run(["schtasks", "/End", "/TN", TASK_NAME], check=False, capture_output=True)
        return
    if sys.platform == "darwin":
        uid = os.getuid()
        subprocess.run(["launchctl", "bootout", f"gui/{uid}/{MAC_LABEL}"], check=False, capture_output=True)
        return
    subprocess.run(["systemctl", "--user", "stop", UNIT_NAME], check=False, capture_output=True)


def windows_startup_dir():
    return Path.home() / "AppData" / "Roaming" / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"


def install_windows_startup(python):
    folder = windows_startup_dir()
    folder.mkdir(parents=True, exist_ok=True)
    vbs = folder / "ClaudiosBirth.vbs"
    vbs.write_text(
        "\r\n".join([
            "Set shell = CreateObject(\"Wscript.Shell\")",
            f"shell.CurrentDirectory = \"{ROOT}\"",
            "Do",
            f"  shell.Run \"\"\"{python}\"\" \"\"{BRIDGE}\"\" --daemon\", 0, True",
            "  WScript.Sleep 5000",
            "Loop",
            "",
        ]),
        encoding="utf-8",
    )
    subprocess.Popen(
        ["wscript.exe", str(vbs)],
        cwd=str(ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    print(f"Inicio de Windows: {vbs}")
    print("Arranca con la sesion y vuelve a levantarse si se cierra.")
    return True


def install_windows(python):
    xml = f"""<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Daemon de Claudio's Birth. Reconoce el ESP32 en el Wi-Fi y reenvia las notificaciones.</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>true</Hidden>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>999</Count>
    </RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{xml_escape(str(python))}</Command>
      <Arguments>&quot;{xml_escape(str(BRIDGE))}&quot; --daemon</Arguments>
      <WorkingDirectory>{xml_escape(str(ROOT))}</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"""
    xml_path = ROOT / "claudios-birth-task.xml"
    xml_path.write_text(xml, encoding="utf-16")
    created = subprocess.run(
        ["schtasks", "/Create", "/TN", TASK_NAME, "/XML", str(xml_path), "/F"],
        capture_output=True,
        text=True,
    )
    if created.returncode != 0:
        detail = (created.stderr or created.stdout or "").strip()
        if detail:
            print(detail)
        print("Sin permiso para el Programador de tareas. El daemon queda en la carpeta Inicio.")
        return install_windows_startup(python)
    rule = subprocess.run(
        [
            "netsh", "advfirewall", "firewall", "add", "rule",
            "name=Claudio's Birth beacon",
            "dir=in", "action=allow",
            f"program={python}",
            "enable=yes", "profile=private,domain",
        ],
        capture_output=True,
        text=True,
    )
    started = subprocess.run(["schtasks", "/Run", "/TN", TASK_NAME], capture_output=True, text=True)
    if started.returncode != 0:
        raise SystemExit(started.stderr.strip() or started.stdout.strip() or "no pude arrancar la tarea")
    print(f"Tarea de Windows: {TASK_NAME}")
    print("Arranca al iniciar sesion y se reinicia si se cae.")
    if rule.returncode != 0:
        print("El firewall no dejo abrir UDP 8766. Si el dispositivo no aparece, permite pythonw en redes privadas.")
    return True


def install_mac(python):
    plist_path = Path.home() / "Library" / "LaunchAgents" / f"{MAC_LABEL}.plist"
    plist_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "Label": MAC_LABEL,
        "ProgramArguments": [str(python), str(BRIDGE), "--daemon"],
        "WorkingDirectory": str(ROOT),
        "RunAtLoad": True,
        "KeepAlive": True,
        "StandardOutPath": str(ROOT / "bridge.log"),
        "StandardErrorPath": str(ROOT / "bridge.log"),
    }
    plist_path.write_bytes(plistlib.dumps(payload))
    uid = os.getuid()
    subprocess.run(["launchctl", "bootout", f"gui/{uid}", str(plist_path)], check=False, capture_output=True)
    loaded = subprocess.run(["launchctl", "bootstrap", f"gui/{uid}", str(plist_path)], capture_output=True, text=True)
    if loaded.returncode != 0:
        raise SystemExit(loaded.stderr.strip() or "no pude cargar el LaunchAgent")
    subprocess.run(["launchctl", "enable", f"gui/{uid}/{MAC_LABEL}"], check=False)
    subprocess.run(["launchctl", "kickstart", "-k", f"gui/{uid}/{MAC_LABEL}"], check=False)
    print(f"LaunchAgent: {plist_path}")
    return True


def install_linux(python):
    unit_dir = Path.home() / ".config" / "systemd" / "user"
    unit_dir.mkdir(parents=True, exist_ok=True)
    unit_path = unit_dir / UNIT_NAME
    unit_path.write_text(
        "\n".join([
            "[Unit]",
            "Description=Claudio's Birth bridge",
            "After=network-online.target",
            "",
            "[Service]",
            "Type=simple",
            f"WorkingDirectory={ROOT}",
            f"ExecStart={python} {BRIDGE} --daemon",
            "Restart=always",
            "RestartSec=5",
            "",
            "[Install]",
            "WantedBy=default.target",
            "",
        ]),
        encoding="utf-8",
    )
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=False)
    enabled = subprocess.run(
        ["systemctl", "--user", "enable", "--now", UNIT_NAME],
        capture_output=True,
        text=True,
    )
    if enabled.returncode != 0:
        raise SystemExit(enabled.stderr.strip() or "no pude activar el servicio de usuario")
    linger = subprocess.run(["loginctl", "enable-linger", os.environ.get("USER", "")], check=False, capture_output=True)
    print(f"Servicio de usuario: {unit_path}")
    if linger.returncode != 0:
        print("Sin linger el daemon corre mientras la sesion este abierta.")
    return True


def install():
    python = python_for_daemon()
    if not BRIDGE.exists():
        raise SystemExit(f"No encuentro {BRIDGE}")
    stop_running_bridge()
    time.sleep(0.4)
    if sys.platform == "win32":
        install_windows(python)
    elif sys.platform == "darwin":
        install_mac(python)
    else:
        install_linux(python)
    for _ in range(20):
        time.sleep(0.25)
        state = health()
        if state and state.get("ok"):
            name = state.get("device_name") or "todavia no aparece en el Wi-Fi"
            host = state.get("esp_host") or ""
            extra = f" en {host}" if host else ""
            print(f"Daemon activo. Dispositivo: {name}{extra}")
            return
    print(f"La tarea quedo instalada. El log esta en {ROOT / 'bridge.log'}")


def remove():
    stop_running_bridge()
    if sys.platform == "win32":
        subprocess.run(["schtasks", "/Delete", "/TN", TASK_NAME, "/F"], check=False)
        xml_path = ROOT / "claudios-birth-task.xml"
        if xml_path.exists():
            xml_path.unlink()
        vbs = windows_startup_dir() / "ClaudiosBirth.vbs"
        if vbs.exists():
            vbs.unlink()
        print("Daemon de Windows eliminado.")
        return
    if sys.platform == "darwin":
        plist_path = Path.home() / "Library" / "LaunchAgents" / f"{MAC_LABEL}.plist"
        if plist_path.exists():
            plist_path.unlink()
        print("LaunchAgent eliminado.")
        return
    subprocess.run(["systemctl", "--user", "disable", UNIT_NAME], check=False)
    unit_path = Path.home() / ".config" / "systemd" / "user" / UNIT_NAME
    if unit_path.exists():
        unit_path.unlink()
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=False)
    print("Servicio de usuario eliminado.")


def status():
    state = health()
    if not state:
        print("El daemon no responde en 127.0.0.1:8765")
        return 1
    print(json.dumps(state, ensure_ascii=False, indent=2))
    return 0


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "install"
    if action == "remove":
        remove()
        return 0
    if action == "status":
        return status()
    if action != "install":
        print("uso: python install_daemon.py [install|remove|status]")
        return 1
    install()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
