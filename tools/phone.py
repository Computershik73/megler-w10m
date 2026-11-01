r"""Talk to the phone's Windows Device Portal (USB: http://127.0.0.1:10080, or another port of the IP-over-USB service).

podium-w10m's and gecko-w10m's tools/phone.py, for Megler. The phone must be in
Developer mode with the Device Portal on, connected over USB or reachable over
Wi-Fi, and must already trust the signing certificate
(app/Megler/AppPackages/Megler.cer) and be interop-unlocked (the package asks
for rescap:codeGeneration).

Usage:
    python tools/phone.py version                # the installed Megler package
    python tools/phone.py install [APPX] [--portal]
                                                 # install or update (newest ARM appx by default; app data kept;
                                                 # --portal: through the Device Portal even when on USB)
    python tools/phone.py launch                 # start Megler
    python tools/phone.py stop                   # stop Megler
    python tools/phone.py ls PATH                # list e.g. "\LocalState\apks"
    python tools/phone.py rm PATH                # delete a file under LocalAppData
    python tools/phone.py push FILE... --to PATH [--folder Documents]
                                                 # upload files (an APK to add through the app's picker)
    python tools/phone.py logs [--dir DIR]       # megler.log, megler.prev.log, the checks' logs, settings.ini
    python tools/phone.py mem [--watch SEC] [--top N]
                                                 # memory of Megler, the phone and the GPU
    python tools/phone.py auto [APPX] [--timeout MIN] [--no-install]
                                                 # the unattended run of a --auto build (see below)
    python tools/phone.py summary DIR            # the summary of an auto run's pulled logs again

auto: for a package built with tools/build-appx.sh arm --auto probe|selftest|probe,selftest.
Installs it (the newest ARM appx by default), launches it, waits until
LocalState/megler-auto.log says AUTO DONE (or the app is gone, or --timeout
minutes pass), pulls every log into build/phone/<timestamp>/ and prints them:
the run's own lines, the probe's answers, the self-test's failures and its
last lines, and anything the core logged as a warning or an error.
"""
import argparse
import base64
import concurrent.futures
import glob
import http.cookiejar
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# The phone this tool may touch, and no other: with several phones on USB the
# IP-over-USB service forwards its ports to just one of them (a Lumia 650 once
# answered 127.0.0.1:10443 while the Elite x3 was meant), so every portal is
# checked by its ComputerName before use, and installs go to the device whose
# model WinAppDeployCmd reports, by GUID. Override with these variables.
TARGET_NAME = os.environ.get("MEGLER_PHONE_NAME", "mc-0011fa4f4eb9")  # the HP Elite x3
TARGET_MODEL = os.environ.get("MEGLER_PHONE_MODEL", "HP Elite x3")
TARGET_IP = os.environ.get("MEGLER_PHONE_IP", "192.168.31.226")       # its Wi-Fi address

# The phone is on the local network or on USB, never behind a proxy: a VPN
# client's HTTP(S)_PROXY on this PC once turned every portal request into a
# 503 (or nothing) while a browser, bypassing it for local addresses, loaded
# the portal fine. Every request below goes direct, curl's included.
for _var in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
    os.environ.pop(_var, None)
os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"


def _portal_name(url):
    import ssl as _s
    ctx = _s.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = _s.CERT_NONE
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=ctx))
        with opener.open(url + "/api/os/info", timeout=5) as r:
            return json.loads(r.read()).get("ComputerName")
    except Exception:
        return None


def _usb_ports():
    """The ports the IP-over-USB service listens on. It forwards a phone's
    portal to 10080/10443 or, as for a Lumia 650 in 2026-11, to ports of its
    own choosing (59366 for HTTP, 59365 for HTTPS)."""
    try:
        tasks = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq IpOverUsbSvc.exe"],
                               capture_output=True, text=True, timeout=30).stdout
        pids = {line.split('","')[1] for line in tasks.splitlines() if line.startswith('"IpOverUsbSvc')}
        lines = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError, IndexError):
        return []
    ports = set()
    for line in lines.splitlines():
        f = line.split()
        if len(f) == 5 and f[3] == "LISTENING" and f[4] in pids:
            ports.add(int(f[1].rsplit(":", 1)[1]))
    return sorted(ports - {10080, 10443})


def _find_portal(patience=None):
    """The target phone's Device Portal: USB (HTTP 10080 or HTTPS 10443, or
    another port of the IP-over-USB service) if it is the one forwarded there,
    else its Wi-Fi address. Only a portal whose ComputerName is TARGET_NAME is
    ever used. A phone's Wi-Fi portal comes and goes as the screen sleeps, so
    keep asking for up to `patience` seconds (MEGLER_PHONE_PATIENCE, default
    300)."""
    if patience is None:
        patience = float(os.environ.get("MEGLER_PHONE_PATIENCE", "300"))
    candidates = []
    if os.environ.get("MEGLER_PORTAL"):
        candidates.append(os.environ["MEGLER_PORTAL"])
    candidates += ["http://127.0.0.1:10080", "https://127.0.0.1:10443"]
    for port in _usb_ports():
        candidates += [f"http://127.0.0.1:{port}", f"https://127.0.0.1:{port}"]
    candidates += [f"https://{TARGET_IP}", f"http://{TARGET_IP}"]
    deadline = time.time() + patience
    while True:
        seen = []
        # All at once: some of the service's ports take a connection and never answer.
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            names = list(pool.map(_portal_name, candidates))
        for url, name in zip(candidates, names):
            if name == TARGET_NAME:
                return url
            if name:
                seen.append(f"{url} is {name}")
        if time.time() >= deadline:
            sys.exit(f"the target phone {TARGET_NAME} ({TARGET_MODEL}) isn't reachable"
                     + (f"; other phones answered: {', '.join(seen)}" if seen else ""))
        time.sleep(10)


_portal = None


def portal():
    global _portal
    if _portal is None:
        _portal = _find_portal()
    return _portal


PACKAGE_PREFIX = "Megler_"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPX_DIR = os.path.join(ROOT, "app", "Megler", "AppPackages")
# Files in the package's LocalAppData, by their folder under it.
LOGS = [
    ("\\LocalState", "megler.log"),
    ("\\LocalState", "megler.prev.log"),
    ("\\LocalState", "megler.prev2.log"),
    ("\\LocalState", "megler-boot.txt"),
    ("\\LocalState", "megler-probe.log"),
    ("\\LocalState", "megler-probe-steps.log"),
    ("\\LocalState", "megler-selftest.log"),
    ("\\LocalState", "megler-selftest-steps.log"),
    ("\\LocalState", "megler-auto.log"),
    ("\\LocalState", "settings.ini"),
]

_cookies = http.cookiejar.CookieJar()
import ssl

# The portal's certificate is self-signed by the phone.
_ssl = ssl.create_default_context()
_ssl.check_hostname = False
_ssl.verify_mode = ssl.CERT_NONE
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                      urllib.request.HTTPCookieProcessor(_cookies),
                                      urllib.request.HTTPSHandler(context=_ssl))


def _with_retries(action):
    """The Wi-Fi portal drops requests while the phone dozes: retry for a
    while before giving up."""
    deadline = time.time() + float(os.environ.get("MEGLER_PHONE_PATIENCE", "300"))
    while True:
        try:
            return action()
        except urllib.error.HTTPError:
            raise  # the portal answered; a status is an answer, not a dropout
        except (urllib.error.URLError, OSError, TimeoutError):
            if time.time() >= deadline:
                raise
            time.sleep(10)


def get(endpoint, **params):
    url = portal() + endpoint + ("?" + urllib.parse.urlencode(params) if params else "")

    def attempt():
        with _opener.open(url, timeout=30) as r:
            return r.read()

    return _with_retries(attempt)


def csrf_token():
    # The portal wants its CSRF cookie echoed back in a header on anything
    # that changes state; a GET first makes it hand one out.
    get("/api/os/info")
    return next((c.value for c in _cookies if c.name == "CSRF-Token"), "")


def post(endpoint, method="POST", **params):
    token = csrf_token()
    url = portal() + endpoint + ("?" + urllib.parse.urlencode(params) if params else "")
    request = urllib.request.Request(url, data=b"", method=method, headers={"X-CSRF-Token": token})
    with _opener.open(request, timeout=30) as r:
        return r.read()


def package(required=True):
    pkgs = json.loads(get("/api/app/packagemanager/packages"))["InstalledPackages"]
    for p in pkgs:
        if p["PackageFullName"].startswith(PACKAGE_PREFIX):
            return p
    if required:
        sys.exit("Megler is not installed on the phone")
    return None


def cmd_version(_):
    print(package()["PackageFullName"])


def winappdeploycmd():
    found = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x86\WinAppDeployCmd.exe"))
    if not found:
        sys.exit("WinAppDeployCmd.exe not found in the Windows Kits")
    # 10.0.16299's is the one known to talk to Windows 10 Mobile.
    for path in found:
        if "10.0.16299.0" in path:
            return path
    return found[-1]


def target_guid():
    """The WinAppDeployCmd GUID of the device whose model is TARGET_MODEL; never
    an address that could reach another phone."""
    r = subprocess.run([winappdeploycmd(), "devices"], capture_output=True, text=True, timeout=120)
    matches = [line.split("\t") for line in r.stdout.splitlines() if line.strip().endswith(TARGET_MODEL)]
    if not matches:
        return None  # not on USB: install through its Device Portal instead
    if len(matches) != 1:
        sys.exit(f"expected exactly one {TARGET_MODEL} in WinAppDeployCmd devices, got:\n{r.stdout}")
    return matches[0][1].strip()


def newest_appx():
    candidates = glob.glob(os.path.join(APPX_DIR, "Megler_*_ARM.appx"))
    if not candidates:
        sys.exit(f"no Megler_*_ARM.appx in {APPX_DIR}; run tools/build-appx.sh first")
    return max(candidates, key=os.path.getmtime)


def cmd_install(a):
    appx = a.appx or newest_appx()
    try:
        installed = package(required=False)
    except Exception as e:
        sys.exit(f"the Device Portal at {portal()} doesn't answer ({e}); is the phone connected?")
    # update keeps the app's data (the library, each app's saves); install is
    # for the first time. The package has no framework dependencies to send
    # along: the shell's C runtime is linked in and ANGLE's is inside it.
    verb = "update" if installed else "install"
    print(f"{verb}: {os.path.basename(appx)}")
    # WinAppDeployCmd answers E_ACCESSDENIED for a phone it was not paired
    # with (a Lumia 650 in 2026-11) while its portal on USB installs fine.
    guid = None if a.portal else target_guid()
    if guid is None:
        return install_through_portal(appx)
    r = subprocess.run([winappdeploycmd(), verb, "-file", appx, "-g", guid],
                       capture_output=True, text=True, errors="replace")
    out = (r.stdout + r.stderr).strip()
    line = out.splitlines()[-1] if out else ""
    encoding = sys.stdout.encoding or "utf-8"
    print(line.encode(encoding, errors="replace").decode(encoding))
    if r.returncode == 0:
        print("now on the phone:", package()["PackageFullName"])
    return r.returncode


def install_through_portal(appx):
    """Installs (or updates, keeping the app's data) through the target
    phone's Device Portal -- for when it is reachable over Wi-Fi but not on
    USB, or on USB with install --portal. The certificate beside the appx
    goes along, so a phone that has never had Megler comes to trust it. The
    portal answers the state query 204 while it is still installing."""
    print(f"installing through the Device Portal of {TARGET_MODEL} at {portal()}")
    token = csrf_token()
    cookie = "; ".join(f"{c.name}={c.value}" for c in _cookies)
    name = os.path.basename(appx)
    parts = ["-F", f'{name}=@"{appx}"']
    cer = os.path.join(os.path.dirname(os.path.abspath(appx)), "Megler.cer")
    if os.path.exists(cer):
        parts += ["-F", f'Megler.cer=@"{cer}"']
    r = subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "-o", "-", "-w", "\nHTTP %{http_code}", "-X", "POST",
                        "-H", f"X-CSRF-Token: {token}", "-H", f"Cookie: {cookie}", *parts,
                        f"{portal()}/api/app/packagemanager/package?{urllib.parse.urlencode({'package': name})}"],
                       capture_output=True, text=True, timeout=1800)
    status = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    if r.returncode or not status.endswith(("200", "202", "204")):
        print(r.stdout.strip(), r.stderr.strip())
        return 1
    deadline = time.time() + 900
    while time.time() < deadline:
        time.sleep(5)
        try:
            request = urllib.request.Request(portal() + "/api/app/packagemanager/state")
            with _opener.open(request, timeout=30) as resp:
                code = resp.status
                body = resp.read()
        except urllib.error.HTTPError as e:
            print("install failed:", e.code, e.read()[:500])
            return 1
        except (urllib.error.URLError, OSError):
            continue
        if code == 204:
            continue
        print("install state:", body[:300].decode("utf-8", "replace"))
        break
    p = package(required=False)
    print("now on the phone:", p["PackageFullName"] if p else "(nothing)")
    # Megler_0.1.0.7_ARM.appx is on the phone as Megler_0.1.0.7_arm__<publisher id>.
    wanted = os.path.splitext(name)[0].replace("Megler_", PACKAGE_PREFIX).rsplit("_ARM", 1)[0]
    return 0 if p and p["PackageFullName"].startswith(wanted) else 1


def cmd_launch(_):
    p = package()
    b64 = lambda s: base64.b64encode(s.encode("utf-8")).decode("ascii")
    # (The portal wants the package-relative id, "<family>!App", not the bare application id.)
    post("/api/taskmanager/app", appid=b64(p.get("PackageRelativeId") or "App"), package=b64(p["PackageFullName"]))
    print("launched", p["PackageFullName"])


def cmd_stop(_):
    p = package()
    b64 = lambda s: base64.b64encode(s.encode("utf-8")).decode("ascii")
    post("/api/taskmanager/app", method="DELETE", package=b64(p["PackageFullName"]))
    print("stopped", p["PackageFullName"])


def app_files(path):
    return json.loads(get("/api/filesystem/apps/files", knownfolderid="LocalAppData",
                          packagefullname=package()["PackageFullName"], path=path)).get("Items", [])


def cmd_ls(a):
    for f in app_files(a.path):
        kind = "dir " if f.get("Type") == 16 else "file"
        print(f"{kind} {f.get('FileSize') or '':>12} {f['Name']}")


def cmd_rm(a):
    # Either kind of slash, with or without the leading one: the Device Portal answers 500 to a path that does not start with a backslash.
    folder, _, name = ("\\" + a.path.replace("/", "\\").strip("\\")).rpartition("\\")
    post("/api/filesystem/apps/file", method="DELETE", knownfolderid="LocalAppData",
         packagefullname=package()["PackageFullName"], path=folder, filename=name)
    print("removed", a.path)


def cmd_push(a):
    """Uploads a local file into a folder of the phone (curl streams it; the
    portal's own multipart upload)."""
    token = csrf_token()
    cookie = "; ".join(f"{c.name}={c.value}" for c in _cookies)
    params = {"knownfolderid": a.folder, "path": a.to}
    if a.folder == "LocalAppData":
        params["packagefullname"] = package()["PackageFullName"]
    query = urllib.parse.urlencode(params)
    for local in a.files:
        size = os.path.getsize(local)
        print(f"push {local} ({size // 2**20} MB) -> {a.folder} {a.to}")
        r = subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "--fail", "-X", "POST", "-H", f"X-CSRF-Token: {token}",
                            "-H", f"Cookie: {cookie}", "-F", f'file=@"{local}"',
                            f"{portal()}/api/filesystem/apps/file?{query}"])
        if r.returncode:
            sys.exit(f"upload of {local} failed (curl exit {r.returncode})")


def cmd_logs(a):
    pfn = package()["PackageFullName"]
    out_dir = a.dir or os.path.join(ROOT, "build", "phone")
    os.makedirs(out_dir, exist_ok=True)
    for folder, name in LOGS:
        try:
            data = get("/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=pfn, filename=name,
                       path=folder)
        except Exception as e:
            print(f"{name}: {e}")
            continue
        out = os.path.join(out_dir, name)
        with open(out, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes -> {out}")


def cmd_shots(a):
    """Fetches the screenshots an auto run made (flag "shots", flags "tap-S-X-Y":
    app/Megler/auto/AutoRun.cpp) as PNGs, and removes them from the phone
    unless --keep."""
    pfn = package()["PackageFullName"]
    out_dir = a.dir or os.path.join(ROOT, "build", "phone", "shots")
    os.makedirs(out_dir, exist_ok=True)
    names = sorted(f["Name"] for f in app_files("\\LocalState") if f["Name"].startswith("shot-") and f["Name"].endswith(".bmp"))
    for name in names:
        data = get("/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=pfn, filename=name, path="\\LocalState")
        out = os.path.join(out_dir, name)
        with open(out, "wb") as f:
            f.write(data)
        try:
            from PIL import Image
            png = out[:-4] + ".png"
            Image.open(out).save(png)
            os.remove(out)
            out = png
        except Exception as e:  # no Pillow: the BMP will do
            print(f"(kept as BMP: {e})")
        print(out)
        if not a.keep:
            post("/api/filesystem/apps/file", method="DELETE", knownfolderid="LocalAppData", packagefullname=pfn,
                 path="\\LocalState", filename=name)
    if not names:
        print("no screenshots on the phone")


def mb(v):
    return v / 2**20


def cmd_mem(a):
    while True:
        stamp = time.strftime("%H:%M:%S")
        procs = json.loads(get("/api/resourcemanager/processes"))["Processes"]
        perf = json.loads(get("/api/resourcemanager/systemperf"))
        page = perf.get("PageSize", 4096)
        total = perf.get("TotalPages", 0) * page
        avail = perf.get("AvailablePages", 0) * page
        commit = perf.get("CommittedPages", 0) * page
        limit = perf.get("CommitLimit", 0) * page
        ours = [p for p in procs if (p.get("PackageFullName") or "").startswith(PACKAGE_PREFIX)]
        if not ours:
            print(f"{stamp} Megler is not running")
        for p in ours:
            print(f"{stamp} Megler: private {mb(p.get('PrivateWorkingSet', 0)):.0f} MB, "
                  f"working set {mb(p.get('WorkingSetSize', 0)):.0f} MB, "
                  f"commit {mb(p.get('TotalCommit', 0)):.0f} MB, "
                  f"virtual {mb(p.get('VirtualSize', 0)):.0f} MB, cpu {p.get('CPUUsage', 0):.0f}%")
        print(f"{stamp} phone: {mb(avail):.0f} MB free of {mb(total):.0f} MB, "
              f"committed {mb(commit):.0f} of {mb(limit):.0f} MB, cpu {perf.get('CpuLoad', 0)}%")
        for gpu in (perf.get("GPUData") or {}).get("AvailableAdapters", []):
            engines = ", ".join(f"{e:.0f}" for e in gpu.get("EnginesUtilization", [])[:4])
            print(f"{stamp} gpu {gpu.get('Description')}: shared memory used "
                  f"{mb(gpu.get('SystemMemoryUsed', 0)):.0f} of {mb(gpu.get('SystemMemory', 0)):.0f} MB, "
                  f"dedicated {mb(gpu.get('DedicatedMemoryUsed', 0)):.0f} of "
                  f"{mb(gpu.get('DedicatedMemory', 0)):.0f} MB, engines % {engines}")
        if a.top:
            print(f"{stamp} top {a.top} by private working set:")
            for p in sorted(procs, key=lambda p: p.get("PrivateWorkingSet", 0), reverse=True)[:a.top]:
                print(f"    {mb(p.get('PrivateWorkingSet', 0)):6.0f} MB  {p.get('ImageName')}")
        if not a.watch:
            break
        time.sleep(a.watch)


# MARK: auto -- the unattended run of a --auto build (app/Megler/auto)

AUTO_LOGS = ["megler-auto.log", "megler-probe.log", "megler-probe-steps.log", "megler-selftest.log", "megler-selftest-steps.log",
             "megler.log", "megler.prev.log", "megler.prev2.log", "megler-boot.txt", "settings.ini"]
# The logs an auto run starts afresh (auto/AutoRun.cpp), the core's step logs
# included: what tells this run's from an earlier one's.
RUN_LOGS = ("megler-auto.log", "megler-probe.log", "megler-probe-steps.log", "megler-selftest.log", "megler-selftest-steps.log")


def try_get(endpoint, **params):
    """get(), or None while the portal doesn't answer (it drops for a minute
    or two after a launch)."""
    try:
        return get(endpoint, **params)
    except Exception:
        return None


def megler_running():
    """True/False from the process list, None when the portal doesn't answer."""
    data = try_get("/api/resourcemanager/processes")
    if data is None:
        return None
    try:
        procs = json.loads(data)["Processes"]
    except Exception:
        return None
    return any((p.get("PackageFullName") or "").startswith(PACKAGE_PREFIX) for p in procs)


def read_app_file(pfn, name, folder="\\LocalState"):
    return try_get("/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=pfn, filename=name,
                   path=folder)


def launch_and_confirm(pfn, wait=240):
    """Launches Megler and waits until the process list shows it. The portal
    can answer the launch with HTTP 500 although it worked, and then stops
    answering for 25-135 s, so only the process list is believed."""
    b64 = lambda s: base64.b64encode(s.encode("utf-8")).decode("ascii")
    for attempt in (1, 2):
        try:
            post("/api/taskmanager/app", appid=b64((package(required=False) or {}).get("PackageRelativeId") or "App"), package=b64(pfn))
            print("launch: requested")
        except Exception as e:
            print(f"launch: the portal answered {e} (it may have worked all the same)")
        deadline = time.time() + wait
        while time.time() < deadline:
            time.sleep(5)
            running = megler_running()
            if running:
                print("launch: Megler is running")
                return True
            if running is None:
                print("  (the portal isn't answering yet)")
        print(f"launch: not running after {wait} s" + (", trying once more" if attempt == 1 else ""))
    return False


def cmd_auto(a):
    appx = a.appx or newest_appx()
    if not a.no_install:
        a.appx = appx
        if cmd_install(a):
            sys.exit("install failed")
    pkg = package()
    pfn = pkg["PackageFullName"]
    print("package:", pfn)
    # A run already going (the user opened the app -- the portal can't launch
    # it on a locked or busy phone) is attached to, never ended: stopping it
    # here once killed a run the user had just started by hand.
    attach = a.no_install and megler_running() is True
    if attach:
        # An auto run goes on after its AUTO DONE (the app is left as it is):
        # attached to, it would hand its old logs over as this run's. Whose it
        # is now this can't tell, so it is left running all the same.
        done_before = read_app_file(pfn, "megler-auto.log") or b""
        if b"AUTO DONE" in done_before:
            sys.exit("Megler is running, but its auto run already wrote AUTO DONE: that is a finished run, not a new one. "
                     "If it is yours, end it (python tools/phone.py stop) and run this again.")
        print("Megler is already running: attaching to its run")
    else:
        try:
            post("/api/taskmanager/app", method="DELETE", package=base64.b64encode(pfn.encode()).decode())
        except Exception:
            pass
    # What an earlier run left: an update keeps LocalState, so until this
    # run has started its logs afresh (the app does, for every stage's log,
    # once it is up) a file that still reads the same is not this run's --
    # its AUTO DONE included, should the app die before getting that far.
    stale = {name: read_app_file(pfn, name) for name in RUN_LOGS}

    def fresh(name):
        data = read_app_file(pfn, name)
        return None if data is None or (data and data == stale.get(name)) else data

    if attach:
        stale = {}  # the running app has already started its logs afresh
    elif not launch_and_confirm(pfn):
        sys.exit("Megler didn't start; nothing to collect")
    started = time.time()
    deadline = started + a.timeout * 60
    seen = 0
    gone = 0
    done = False
    while time.time() < deadline:
        time.sleep(5)
        data = fresh("megler-auto.log")
        if data is not None:
            lines = data.decode("utf-8", "replace").splitlines()
            for line in lines[seen:]:
                print("  auto |", line)
            seen = max(seen, len(lines))
            if any("AUTO DONE" in line for line in lines):
                done = True
                break
        running = megler_running()
        gone = gone + 1 if running is False else 0
        if gone >= 3:
            print("Megler is no longer running and never wrote AUTO DONE: it crashed or was ended")
            break
    else:
        print(f"timed out after {a.timeout:g} minutes")
    out_dir = os.path.join(ROOT, "build", "phone", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)
    for name in AUTO_LOGS:
        data = read_app_file(pfn, name)
        if data is None:
            print(f"{name}: not there (or not readable)")
            continue
        if data and data == stale.get(name):
            print(f"{name}: as an earlier run left it (this run never got to it); not copied")
            continue
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
    print(f"logs: {out_dir}")
    summarize(out_dir)
    return 0 if done else 1


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return None


def strip_time(line):
    """Drops the 'HH:MM:SS.mmm ' the logs stamp each line with."""
    return re.sub(r"^\d\d:\d\d:\d\d(\.\d+)? ", "", line)


def summarize(out_dir):
    """Prints (and writes to summary.txt) what an auto run found."""
    summary = []

    def emit(line=""):
        print(line)
        summary.append(line)

    auto = read_lines(os.path.join(out_dir, "megler-auto.log")) or []
    emit("== auto run ==")
    for line in auto:
        emit("  " + strip_time(line))
    if not any("AUTO DONE" in line for line in auto):
        emit("  (no AUTO DONE: the run did not finish)")

    def steps(name, what):
        """A check that never said DONE ended the process on the way (some
        probes can): the core's own step log, flushed line by line, says how
        far it got."""
        lines = read_lines(os.path.join(out_dir, name))
        emit(f"  (no DONE line: the {what} did not finish)")
        if lines:
            emit(f"  the last steps in {name}:")
            for line in lines[-12:]:
                emit("    " + line)

    probe = read_lines(os.path.join(out_dir, "megler-probe.log"))
    if probe:
        # Every line is an answer ("name: value"; docs/ARCHITECTURE.md says
        # what each means): all of them.
        emit("\n== probe ==")
        for line in probe:
            emit("  " + strip_time(line))
        if not any(strip_time(line).startswith("DONE probe") for line in probe):
            steps("megler-probe-steps.log", "probe")

    selftest = read_lines(os.path.join(out_dir, "megler-selftest.log"))
    if selftest:
        emit("\n== selftest ==")
        body = [strip_time(line) for line in selftest]
        if len(body) <= 120:
            for line in body:
                emit("  " + line)
        else:
            # A long run: what failed, then how it ended.
            failed = [line for line in body if re.search(r"\b(FAIL|FAILED|CRASH|FAULT|ABORT)\b", line)]
            emit(f"  {len(body)} lines; {len(failed)} of them say FAIL, CRASH, FAULT or ABORT:")
            for line in failed[:80]:
                emit("    " + line)
            emit("  the last lines:")
            for line in body[-25:]:
                emit("    " + line)
        if not any(line.startswith("DONE selftest") for line in body):
            steps("megler-selftest-steps.log", "self-test")

    core = read_lines(os.path.join(out_dir, "megler.log"))
    if core:
        # megler.log's lines are "HH:MM:SS.mmm L message", L one of D I W E.
        loud = [line for line in core if re.match(r"^\d\d:\d\d:\d\d\.\d+ [WE] ", line) or "FATAL" in line]
        emit(f"\n== megler.log: {len(core)} lines, {len(loud)} warnings and errors ==")
        for line in loud[:60]:
            emit("  " + line)
    with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(summary) + "\n")


def cmd_summary(a):
    summarize(a.dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("version")
    ip = sub.add_parser("install")
    ip.add_argument("appx", nargs="?")
    ip.add_argument("--portal", action="store_true", help="install through the Device Portal even when on USB")
    sub.add_parser("launch")
    lp = sub.add_parser("logs")
    lp.add_argument("--dir")
    mp = sub.add_parser("mem")
    mp.add_argument("--watch", type=float, default=0)
    mp.add_argument("--top", type=int, default=10)
    sub.add_parser("stop")
    sh = sub.add_parser("shots")
    sh.add_argument("--dir")
    sh.add_argument("--keep", action="store_true")
    sub.add_parser("ls").add_argument("path")
    sub.add_parser("rm").add_argument("path")
    pp = sub.add_parser("push")
    pp.add_argument("files", nargs="+")
    pp.add_argument("--to", required=True)
    # Files the portal writes into an app's LocalAppData keep the ACL of the
    # portal's temporary folder, so the app can't open them. Push an APK into
    # Documents instead (--folder Documents --to "\") and add it through the
    # app's file picker.
    pp.add_argument("--folder", default="LocalAppData", help="Device Portal known folder (Documents, ...)")
    au = sub.add_parser("auto")
    au.add_argument("appx", nargs="?")
    au.add_argument("--timeout", type=float, default=15, help="minutes to wait for AUTO DONE (default 15)")
    au.add_argument("--no-install", action="store_true", help="run what is installed")
    sp = sub.add_parser("summary")
    sp.add_argument("dir")
    a = ap.parse_args()
    sys.exit({"version": cmd_version, "install": cmd_install, "launch": cmd_launch, "logs": cmd_logs,
              "mem": cmd_mem, "stop": cmd_stop, "ls": cmd_ls, "rm": cmd_rm, "push": cmd_push, "auto": cmd_auto,
              "summary": cmd_summary, "shots": cmd_shots}[a.cmd](a))


if __name__ == "__main__":
    main()
