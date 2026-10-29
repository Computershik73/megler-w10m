r"""Which of the framework's methods that are signatures alone the apps on the phone call: the "api: ... is a stub here"
lines of each app's own package's logs (megler.log and the two before it), put together.

    python tools/api-stubs.py [--top N] [--app ANDROID_PACKAGE ...] [--dir DIR]

For each method: how many apps called it, and from where (the first caller each app's log names). The most used first:
what to make work next. With --dir the logs are read from a folder pulled before (DIR\<package>\megler*.log) instead
of the phone. Read only.
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LINE = re.compile(r'api: (\S+?)\.([^.(]+)(\([^ ]*) is a stub here \(its signature alone\), first called from (.*)$')


def logs_from_phone(apps):
    import phone
    pkgs = json.loads(phone.get("/api/app/packagemanager/packages"))["InstalledPackages"]
    for p in pkgs:
        pfn = p["PackageFullName"]
        if not pfn.startswith("MeglerApp."):
            continue
        android = pfn.split("_")[0][len("MeglerApp."):].rsplit(".", 1)[0]
        if apps and android not in apps:
            continue
        texts = []
        for name in ("megler.log", "megler.prev.log", "megler.prev2.log"):
            data = phone.read_app_file(pfn, name)
            if data:
                texts.append(data.decode("utf-8", "replace"))
        yield android, "\n".join(texts)


def logs_from_dir(root, apps):
    for android in sorted(os.listdir(root)):
        folder = os.path.join(root, android)
        if not os.path.isdir(folder) or (apps and android not in apps):
            continue
        texts = []
        for name in sorted(os.listdir(folder)):
            if name.startswith("megler") and name.endswith(".log"):
                texts.append(open(os.path.join(folder, name), encoding="utf-8", errors="replace").read())
        yield android, "\n".join(texts)


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("--top", type=int, default=60)
    a.add_argument("--app", nargs="*", default=[])
    a.add_argument("--dir")
    args = a.parse_args()
    sys.argv = [sys.argv[0]]
    by_method = collections.defaultdict(dict)
    apps_seen = 0
    source = logs_from_dir(args.dir, args.app) if args.dir else logs_from_phone(args.app)
    for android, text in source:
        apps_seen += 1
        for line in text.splitlines():
            m = LINE.search(line)
            if m:
                cls, name, sig, caller = m.groups()
                by_method[(cls, name, sig)].setdefault(android, caller.strip())
    ranked = sorted(by_method.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    print("%d apps' logs read; %d stubs called" % (apps_seen, len(ranked)))
    for (cls, name, sig), callers in ranked[:args.top]:
        print("%3d  %s.%s%s" % (len(callers), cls.replace("/", "."), name, sig))
        for android, caller in sorted(callers.items())[:4]:
            print("       %s: %s" % (android, caller))


main()
