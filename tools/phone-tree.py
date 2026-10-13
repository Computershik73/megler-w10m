r"""List a folder of Megler's on the phone, with everything under it.

    python tools/phone-tree.py "LocalState/apps/<package>" [--depth 6]

Either kind of slash. Sizes in bytes; folders end in a slash.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phone  # noqa: E402


def walk(path, depth, indent=""):
    try:
        items = phone.app_files(path)
    except Exception as e:
        print(f"{indent}(cannot list: {e})")
        return
    for f in sorted(items, key=lambda f: f["Name"].lower()):
        if f.get("Type") == 16:
            print(f"{indent}{f['Name']}/")
            if depth > 0:
                walk(path + "\\" + f["Name"], depth - 1, indent + "  ")
        else:
            print(f"{indent}{f['Name']}  {f.get('FileSize', '')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--depth", type=int, default=6)
    a = ap.parse_args()
    walk("\\" + a.path.replace("/", "\\").strip("\\"), a.depth)


if __name__ == "__main__":
    main()
