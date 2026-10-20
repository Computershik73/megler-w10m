r"""Set, list or clear Megler's flag files on the phone (LocalState\flags).

    python tools/phone-flags.py list
    python tools/phone-flags.py set shots play-170 tap-100-640-424
    python tools/phone-flags.py clear            # removes every flag
    python tools/phone-flags.py only shots ...   # clear, then set
    python tools/phone-flags.py --app com.instagram.android set trace-net
                                                 # an app's own package's flags

A flag is a file whose name is all that matters (core: mg_flag; shell:
auto/AutoRun.cpp for shots, tap-S-X-Y, key-S-C, play-N). Each package reads
its own LocalState\flags: with --app, the package Megler made of that Android
app (docs/PER-APP-PACKAGES.md), found by its identity name, "MeglerApp." and
the Android package. This goes through Python end to end: a shell that eats
backslashes cannot mangle the paths.
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402

FOLDER = "\\LocalState\\flags"
PFN = None  # the package whose flags these are: Megler's, or an app's own


def app_package(android_package):
    """The full name of the package Megler made of `android_package`. Its identity name is "MeglerApp." and the package,
    '_' made '-', filled or cut to 50 characters (core/src/appx/mg_appx_manifest.c): the first 41 are the package's."""
    prefix = ("MeglerApp." + android_package.replace("_", "-"))[:41]
    pkgs = json.loads(phone.get("/api/app/packagemanager/packages"))["InstalledPackages"]
    found = [p["PackageFullName"] for p in pkgs if p["PackageFullName"].split("_")[0].startswith(prefix)]
    if len(found) != 1:
        sys.exit(f"{len(found)} packages of {android_package} on the phone" + (": " + ", ".join(found) if found else ""))
    return found[0]


def names():
    try:
        return sorted(f["Name"] for f in json.loads(phone.get("/api/filesystem/apps/files", knownfolderid="LocalAppData",
                                                              packagefullname=PFN, path=FOLDER)).get("Items", []))
    except Exception:
        return []


def remove(name):
    try:
        phone.post("/api/filesystem/apps/file", method="DELETE", knownfolderid="LocalAppData",
                   packagefullname=PFN, path=FOLDER, filename=name)
    except urllib.error.HTTPError as e:
        print(f"could not remove {name}: HTTP {e.code}")


def put(name):
    scratch = os.path.join(phone.ROOT, "build", "phone", "flags-tmp")
    os.makedirs(scratch, exist_ok=True)
    local = os.path.join(scratch, name)
    with open(local, "w") as f:
        f.write("x")
    token = phone.csrf_token()
    cookie = "; ".join(f"{c.name}={c.value}" for c in phone._cookies)
    query = urllib.parse.urlencode({"knownfolderid": "LocalAppData", "path": FOLDER, "packagefullname": PFN})
    # (curl reports 22 for this upload although the file lands; what counts is the listing afterwards.)
    subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "-o", os.devnull, "-X", "POST", "-H", f"X-CSRF-Token: {token}",
                    "-H", f"Cookie: {cookie}", "-F", f'file=@"{local}"', f"{phone.portal()}/api/filesystem/apps/file?{query}"],
                   stderr=subprocess.DEVNULL)


def main():
    global PFN
    args = sys.argv[1:]
    app = None
    if len(args) >= 2 and args[0] == "--app":
        app, args = args[1], args[2:]
    if not args or args[0] not in ("list", "set", "clear", "only"):
        sys.exit(__doc__)
    PFN = app_package(app) if app else phone.package()["PackageFullName"]
    cmd, wanted = args[0], args[1:]
    if cmd in ("clear", "only"):
        for name in names():
            remove(name)
    if cmd in ("set", "only"):
        for name in wanted:
            put(name)
    print(f"flags of {PFN.split('_')[0]} on the phone:", " ".join(names()) or "(none)")


if __name__ == "__main__":
    main()
