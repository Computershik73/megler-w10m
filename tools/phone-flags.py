r"""Set, list or clear Megler's flag files on the phone (LocalState\flags).

    python tools/phone-flags.py list
    python tools/phone-flags.py set shots play-170 tap-100-640-424
    python tools/phone-flags.py clear            # removes every flag
    python tools/phone-flags.py only shots ...   # clear, then set

A flag is a file whose name is all that matters (core: hk_flag; shell:
auto/AutoRun.cpp for shots, tap-S-X-Y, key-S-C, play-N). This goes through
Python end to end: a shell that eats backslashes cannot mangle the paths.
"""
import os
import subprocess
import sys
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402

FOLDER = "\\LocalState\\flags"


def names():
    try:
        return sorted(f["Name"] for f in phone.app_files(FOLDER))
    except Exception:
        return []


def remove(name):
    try:
        phone.post("/api/filesystem/apps/file", method="DELETE", knownfolderid="LocalAppData",
                   packagefullname=phone.package()["PackageFullName"], path=FOLDER, filename=name)
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
    query = urllib.parse.urlencode({"knownfolderid": "LocalAppData", "path": FOLDER,
                                    "packagefullname": phone.package()["PackageFullName"]})
    # (curl reports 22 for this upload although the file lands; what counts is the listing afterwards.)
    subprocess.run(["curl", "--noproxy", "*", "-k", "-sS", "-o", os.devnull, "-X", "POST", "-H", f"X-CSRF-Token: {token}",
                    "-H", f"Cookie: {cookie}", "-F", f'file=@"{local}"', f"{phone.portal()}/api/filesystem/apps/file?{query}"],
                   stderr=subprocess.DEVNULL)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "set", "clear", "only"):
        sys.exit(__doc__)
    cmd, wanted = sys.argv[1], sys.argv[2:]
    if cmd in ("clear", "only"):
        for name in names():
            remove(name)
    if cmd in ("set", "only"):
        for name in wanted:
            put(name)
    print("flags on the phone:", " ".join(names()) or "(none)")


if __name__ == "__main__":
    main()
