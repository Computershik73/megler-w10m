r"""Fetch one file of Meglet's from the phone and print it (or save it with --out).

    python tools/phone-get.py "LocalState/apps/<package>/data/files/Some.txt" [--out FILE]

The path is under the app's LocalAppData folder, with either kind of slash.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--out")
    a = ap.parse_args()
    path = a.path.replace("/", "\\").strip("\\")
    folder, _, name = path.rpartition("\\")
    data = phone.get("/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=phone.package()["PackageFullName"],
                     filename=name, path="\\" + folder)
    if a.out:
        with open(a.out, "wb") as f:
            f.write(data)
        print(f"{len(data)} bytes -> {a.out}")
    else:
        sys.stdout.write(data.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
