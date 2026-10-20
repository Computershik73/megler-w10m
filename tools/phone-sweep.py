"""Run the phone's APKs sequentially using a --auto play build.

Each run gets fresh logs, its APK report and the last screenshot. Existing
automation flags are restored afterwards; app data is never reset.
Usage: python tools/phone-sweep.py [--app PREFIX ...] [--seconds 40] [--out DIR]
"""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import time
from types import SimpleNamespace

import phone

spec = importlib.util.spec_from_file_location("phone_flags", Path(__file__).with_name("phone-flags.py"))
flags = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flags)


def automation(name):
    return name in ("shots", "demo") or name.startswith(("app-", "play-", "tap-", "px-", "key-"))


def flag_names():
    # A failed listing is not an empty flag directory: do not lose the
    # original selection when the portal temporarily stops responding.
    return {item["Name"] for item in phone.app_files(flags.FOLDER)}


def set_flags(wanted):
    existing = flag_names()
    for name in existing:
        if automation(name) and name not in wanted:
            flags.remove(name)
    for name in wanted - existing:
        flags.put(name)
    actual = {name for name in flag_names() if automation(name)}
    if actual != wanted:
        raise RuntimeError(f"flags did not stick: wanted {wanted}, got {actual}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--app", action="append", default=[])
    ap.add_argument("--packaged", action="append", default=[], help="also add a packaged test APK by its stem")
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--out", default="build/phone/sweep-" + time.strftime("%Y%m%d-%H%M%S"))
    args = ap.parse_args()
    if not 10 <= args.seconds <= 3600:
        ap.error("--seconds must be 10..3600")
    os.environ.setdefault("MEGLER_PHONE_PATIENCE", "20")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    pkg = phone.package()
    pfn = pkg["PackageFullName"]
    # The package is fixed for this sweep; do not re-enumerate every installed
    # Windows app for every flag or screenshot request.
    phone.package = lambda required=True: pkg
    items = phone.app_files("\\LocalState\\apks")
    item_names = {i["Name"] for i in items}
    apps = sorted(i["Name"] for i in items if i["Name"].endswith(".apk"))
    apps = sorted(set(apps) | {stem + ".apk" for stem in args.packaged})
    if args.app:
        apps = [name for name in apps if any(name.casefold().startswith(p.casefold()) for p in args.app)]
        if not apps:
            ap.error("no matching APKs")
    original = {name for name in flag_names() if automation(name)}
    (out / "inventory.json").write_text(json.dumps({"package": pkg, "apps": apps, "flags": sorted(original)}, ensure_ascii=False, indent=2), encoding="utf-8")
    results = []
    try:
        for index, name in enumerate(apps, 1):
            print(f"[{index}/{len(apps)}] {name}", flush=True)
            folder = out / (f"{index:02d}-" + re.sub(r"[^\w.-]", "_", name[:-4])[:85])
            folder.mkdir(exist_ok=True)
            report = phone.read_app_file(pfn, name + ".json", "\\LocalState\\apks") if name + ".json" in item_names else None
            if report:
                (folder / "apk.json").write_bytes(report)
            phone.cmd_stop(None)
            set_flags({"shots", f"play-{args.seconds}", "app-" + name[:-4]})
            stale = {n: phone.read_app_file(pfn, n) for n in ("megler.log", "megler-auto.log")}
            # Portal creation dates/sizes do not change when a BMP is
            # overwritten. Archive pre-existing captures before the first
            # run, then clear the generated frame names between runs.
            for item in phone.app_files("\\LocalState"):
                shot = item["Name"]
                if not re.fullmatch(r"shot-\d+\.bmp", shot):
                    continue
                if index == 1:
                    previous = out / "previous-shots"
                    previous.mkdir(exist_ok=True)
                    data = phone.read_app_file(pfn, shot)
                    if data is None:
                        raise RuntimeError("cannot archive " + shot)
                    (previous / shot).write_bytes(data)
                phone.cmd_rm(SimpleNamespace(path="\\LocalState\\" + shot))
            phone.cmd_launch(None)
            deadline = time.monotonic() + args.seconds + 100
            state = "timeout"
            fresh = False
            while time.monotonic() < deadline:
                time.sleep(3)
                data = phone.read_app_file(pfn, "megler-auto.log")
                if data and data != stale["megler-auto.log"]:
                    fresh = True
                    if b"AUTO DONE" in data:
                        state = "done"
                        break
                if phone.megler_running() is False:
                    state = "process-ended"
                    break
            for log in ("megler.log", "megler-auto.log", "megler-boot.txt"):
                data = phone.read_app_file(pfn, log)
                if data is not None and data != stale.get(log):
                    (folder / log).write_bytes(data)
            auto_path = folder / "megler-auto.log"
            auto = auto_path.read_text(encoding="utf-8", errors="replace") if auto_path.exists() else ""
            # A screenshot belongs to this run only if the auto run selected
            # the requested APK; a refused selection can leave old pictures.
            selected = f"play: {name} (" in auto and f"play: {name} (the flag" not in auto
            if selected:
                shots = sorted(i["Name"] for i in phone.app_files("\\LocalState")
                               if re.fullmatch(r"shot-\d+\.bmp", i["Name"]))
                if shots:
                    data = phone.read_app_file(pfn, shots[-1])
                    if data:
                        from PIL import Image
                        Image.open(io.BytesIO(data)).save(folder / "screen.png")
            report = phone.read_app_file(pfn, name + ".json", "\\LocalState\\apks")
            if report:
                (folder / "apk.json").write_bytes(report)
            lines = [line for line in auto.splitlines() if "play:" in line or "DONE net:" in line or "FAIL" in line]
            result = {"apk": name, "state": state, "fresh_auto": fresh, "selected": selected, "folder": str(folder), "summary": lines}
            results.append(result)
            (out / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            print(json.dumps(result, ensure_ascii=True), flush=True)
    finally:
        phone.cmd_stop(None)
        set_flags(original)


if __name__ == "__main__":
    main()
