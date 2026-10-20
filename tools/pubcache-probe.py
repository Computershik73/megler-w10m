"""Runs the publisher cache folder probe (app/PubCacheProbe, built by tools/build-pubcache-probe.sh) on the phone.

    python tools/pubcache-probe.py [--no-install]

Installs MegletProbeA and MegletProbeB through the phone's Device Portal, starts A, then B (which sends A to the
background), 25 seconds apart, ends both, and prints what each wrote in its LocalState\\probe.log.
The probes' logs are appended to, start after start; --fresh removes them first.
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "pubcache-probe")
NAMES = ("MegletProbeA", "MegletProbeB")


def b64(s):
    return base64.b64encode(s.encode("utf-8")).decode("ascii")


def installed(name):
    for p in json.loads(phone.get("/api/app/packagemanager/packages"))["InstalledPackages"]:
        if p["PackageFullName"].startswith(name + "_"):
            return p
    return None


def install(appx):
    token = phone.csrf_token()
    cookie = "; ".join(f"{c.name}={c.value}" for c in phone._cookies)
    name = os.path.basename(appx)
    r = subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "-o", "-", "-w", "\nHTTP %{http_code}", "-X", "POST",
                        "-H", f"X-CSRF-Token: {token}", "-H", f"Cookie: {cookie}", "-F", f'{name}=@"{appx}"',
                        f"{phone.portal()}/api/app/packagemanager/package?{urllib.parse.urlencode({'package': name})}"],
                       capture_output=True, text=True, timeout=600)
    print(f"install {name}: {r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()}")
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(3)
        try:
            state = phone.get("/api/app/packagemanager/state")
        except Exception as e:  # the portal answers 204 while installing, and an error once it failed
            if "204" in str(e):
                continue
            print(f"  state: {e}")
            return
        if state:
            print(f"  state: {state[:200]}")
            return


def launch(p):
    try:
        phone.post("/api/taskmanager/app", appid=b64(p["PackageRelativeId"]), package=b64(p["PackageFullName"]))
        print(f"{time.strftime('%H:%M:%S')} started {p['PackageFullName']}")
    except Exception as e:
        print(f"{time.strftime('%H:%M:%S')} start {p['PackageFullName']}: the portal answered {e} (it may have worked)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--fresh", action="store_true")
    a = ap.parse_args()
    if not a.no_install:
        for n in NAMES:
            install(os.path.join(OUT, n + ".appx"))
    pkgs = {n: installed(n) for n in NAMES}
    for n, p in pkgs.items():
        if not p:
            sys.exit(f"{n} is not installed")
        if a.fresh:
            try:
                phone.post("/api/filesystem/apps/file", method="DELETE", knownfolderid="LocalAppData",
                           packagefullname=p["PackageFullName"], path="\\LocalState", filename="probe.log")
            except Exception:
                pass
    for n in ("MegletProbeA", "MegletProbeB"):
        launch(pkgs[n])
        time.sleep(25)
    # A probe that runs holds its log open, and the portal answers 404 for it: both are ended first.
    for n, p in pkgs.items():
        try:
            phone.post("/api/taskmanager/app", method="DELETE", package=b64(p["PackageFullName"]))
        except Exception as e:
            print(f"stop {n}: {e}")
    time.sleep(5)
    for n, p in pkgs.items():
        log = phone.read_app_file(p["PackageFullName"], "probe.log")
        print(f"\n==== {n}'s probe.log ====")
        print(log.decode("utf-8", "replace") if log else "(not there)")


if __name__ == "__main__":
    main()
