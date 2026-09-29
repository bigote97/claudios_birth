"""CLI: python notify.py cursor approval ["mensaje opcional"]"""

import sys

from client import post_notify


def main():
    if len(sys.argv) < 3:
        print("uso: python notify.py <cursor|claude> <working|approval|finished|slow|error|idle> [mensaje]")
        return 1
    source = sys.argv[1]
    event_type = sys.argv[2]
    message = sys.argv[3] if len(sys.argv) > 3 else None
    ok = post_notify(source, event_type, message)
    print("entregado" if ok else "en cola o sin bridge")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
