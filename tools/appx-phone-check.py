"""Tries a package made and signed by the core (core/src/appx/hk_appx.c, through tools/appx-tool) on the phone.

    python tools/appx-phone-check.py

Takes the publisher cache folder probe's staged files (tools/build-pubcache-probe.sh, build/pubcache-probe/stage-A) as a
third package, MegletProbeC ("Probe C"), packs and signs it with appx-tool (key and certificate in build/appx-tool/key,
made on the first use), checks it with tools/appx-verify.py, installs it through the phone's Device Portal with the
certificate, starts it, ends it after 20 seconds and prints its LocalState\\probe.log. A log is the answer: the phone took
the signature, and ran a package with no code integrity catalog.
"""
import base64
import os
import shutil
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402
import importlib  # noqa: E402

probe = importlib.import_module("pubcache-probe")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "build", "appx-tool")
STAGE = os.path.join(TOOL, "stage-C")
KEYS = os.path.join(TOOL, "key")


def main():
    src = os.path.join(ROOT, "build", "pubcache-probe", "stage-A")
    if not os.path.isdir(src):
        sys.exit("build the probe first: bash tools/build-pubcache-probe.sh")
    if not os.path.exists(os.path.join(TOOL, "appx-tool.exe")):
        subprocess.run(["bash", os.path.join(ROOT, "tools", "build-appx-tool.sh")], check=True)
    shutil.rmtree(STAGE, ignore_errors=True)
    shutil.copytree(src, STAGE)
    manifest = os.path.join(STAGE, "AppxManifest.xml")
    text = open(manifest, encoding="utf-8").read()
    text = (text.replace('Name="MegletProbeA"', 'Name="MegletProbeC"')
                .replace("6a1f3b2e-5c4d-4e7f-9a8b-1c2d3e4f5a01", "6a1f3b2e-5c4d-4e7f-9a8b-1c2d3e4f5a03")
                .replace("Probe A", "Probe C"))
    open(manifest, "w", encoding="utf-8", newline="\n").write(text)
    os.makedirs(KEYS, exist_ok=True)
    appx = os.path.join(TOOL, "MegletProbeC.appx")
    cer = os.path.join(KEYS, "signer.cer")
    subprocess.run([os.path.join(TOOL, "appx-tool.exe"), STAGE, appx, os.path.join(KEYS, "signer.key"), cer], check=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "appx-verify.py"), appx], check=True)

    token = phone.csrf_token()
    cookie = "; ".join(f"{c.name}={c.value}" for c in phone._cookies)
    name = os.path.basename(appx)
    r = subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "-o", "-", "-w", "\nHTTP %{http_code}", "-X", "POST",
                        "-H", f"X-CSRF-Token: {token}", "-H", f"Cookie: {cookie}",
                        "-F", f'{name}=@"{appx}"', "-F", f'signer.cer=@"{cer}"',
                        f"{phone.portal()}/api/app/packagemanager/package?{urllib.parse.urlencode({'package': name})}"],
                       capture_output=True, text=True, timeout=600)
    print("install:", r.stdout.strip().replace("\n", " "), r.stderr.strip())
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(3)
        try:
            state = phone.get("/api/app/packagemanager/state")
        except Exception as e:
            print("install state:", e)
            return 1
        if state:
            print("install state:", state[:300])
            break
    p = probe.installed("MegletProbeC")
    if not p:
        print("MegletProbeC is not installed")
        return 1
    probe.launch(p)
    time.sleep(20)
    try:
        phone.post("/api/taskmanager/app", method="DELETE", package=base64.b64encode(p["PackageFullName"].encode()).decode())
    except Exception as e:
        print("stop:", e)
    time.sleep(5)
    log = phone.read_app_file(p["PackageFullName"], "probe.log")
    print("==== MegletProbeC's probe.log ====")
    print(log.decode("utf-8", "replace") if log else "(not there: it did not run, or did not get as far as its log)")
    return 0 if log else 1


if __name__ == "__main__":
    sys.exit(main())
