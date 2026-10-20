r"""Record the phone's own event tracing (ETW) while Meglet runs there.

    python tools/phone-etw.py [--seconds 30] [--out build/phone/etw.jsonl]
                              [--launch] [--providers NAME[,NAME...]] [--level 5]

The Windows Device Portal relays events of any provider registered on the
phone over a WebSocket (/api/etw/session/realtime). This asks for the ones
that say what the phone's shell does with an app's window -- shown, hidden,
activated, suspended, ended -- writes every event as one JSON line, and
prints a one-line digest of each as it comes. --launch stops Meglet first and
starts it once the recording runs, so the events cover its whole life.

A provider is named by a part of its name (see --list) or by its GUID.
"""
import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402

import websocket  # websocket-client  # noqa: E402

DEFAULT = [
    "Microsoft-WindowsPhone-CoreUIComponents",
    "Microsoft-WindowsPhone-CoreUI",
    "Microsoft-WindowsPhone-AppModelCore",
    "Microsoft-Windows-AppModel-Exec",
    "Microsoft-WindowsPhone-Shell-Chrome",
    "Microsoft-WindowsPhone-Shell-Events",
    "Microsoft-WindowsPhone-Shell-LockAndWallpaper",
    "Microsoft-Windows-Immersive-Shell",
    "Microsoft-WindowsPhone-Power",
    "Microsoft-WindowsPhone-PowerNotifications",
    "Microsoft.Windows.ResourceManager",
    "Microsoft-Windows-ApplicationResourceManagementSystem",
]


def providers():
    return json.loads(phone.get("/api/etw/providers"))["Providers"]


def resolve(names):
    known = providers()
    out = []
    for name in names:
        hits = [p for p in known if p["Name"].lower() == name.lower() or p["GUID"].lower() == name.lower()]
        if not hits:
            hits = [p for p in known if name.lower() in p["Name"].lower()]
        if not hits:
            print(f"no provider matches {name}", file=sys.stderr)
        out += hits
    return out


def digest(event):
    skip = {"Timestamp", "ProviderName", "ID", "TaskName", "Keyword", "Level", "Opcode", "Version", "ProviderId", "Channel", "Task"}
    rest = {k: v for k, v in event.items() if k not in skip}
    return f'{event.get("ProviderName", "?")} #{event.get("ID", "?")} {event.get("TaskName", "")} {json.dumps(rest, ensure_ascii=False)}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--out", default=os.path.join("build", "phone", "etw.jsonl"))
    ap.add_argument("--launch", action="store_true")
    ap.add_argument("--providers", default="")
    ap.add_argument("--level", type=int, default=5)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    if a.list:
        for p in sorted(providers(), key=lambda p: p["Name"].lower()):
            print(p["GUID"], p["Name"])
        return

    chosen = resolve([n for n in a.providers.split(",") if n] or DEFAULT)
    url = phone.portal().replace("http", "ws", 1) + "/api/etw/session/realtime"
    ws = websocket.create_connection(url, timeout=10, sslopt={"cert_reqs": 0}, origin=phone.portal(),
                                     http_no_proxy=["*"], enable_multithread=True)
    for p in chosen:
        ws.send(f'provider {p["GUID"]} enable {a.level}')
        print(f'recording {p["Name"]}')

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    count = [0]
    done = threading.Event()

    def reader():
        ws.settimeout(1)
        with open(a.out, "w", encoding="utf-8") as f:
            while not done.is_set():
                try:
                    message = ws.recv()
                except websocket.WebSocketTimeoutException:
                    continue
                except Exception as e:  # the phone closed it
                    print(f"recording ended: {e}", file=sys.stderr)
                    return
                if not message:
                    continue
                try:
                    events = json.loads(message).get("Events", [])
                except ValueError:
                    f.write(json.dumps({"raw": message[:2000]}) + "\n")
                    continue
                for e in events:
                    e["_pc_time"] = time.time()
                    f.write(json.dumps(e, ensure_ascii=False) + "\n")
                    count[0] += 1
                    if not a.quiet:
                        print(digest(e)[:400])
                f.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    if a.launch:
        time.sleep(1)
        try:
            phone.cmd_stop(None)
        except Exception as e:
            print(f"stop: {e}", file=sys.stderr)
        time.sleep(2)
        print("--- launching Meglet")
        phone.cmd_launch(None)
    time.sleep(a.seconds)
    done.set()
    t.join(3)
    try:
        ws.close()
    except Exception:
        pass
    print(f"{count[0]} events in {a.out}")


if __name__ == "__main__":
    main()
