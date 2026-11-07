"""Reads the ZIP tools/log-archive-test.cpp had Save All Logs make, with Python's zipfile, and checks what is in it.

    python tools/log-archive-check.py <zip> <the test's LocalState folder>

Every entry's CRC-32; summary.txt first, saying what the stand-ins said and the last install refused; Megler's own files
from its LocalState (not its older copies in the shared folder), the log over 16 MB as its last 16 MB after the line that
says so, its flags; both apps' copies but the one half written (.part); the settings. Exits 0 when all is so.
"""
import pathlib
import sys
import zipfile

CAP = 16 << 20


def main():
    archive, local = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        check(z.testzip() is None, "an entry's CRC-32 does not match its bytes")
        check(names and names[0] == "summary.txt", "summary.txt is not first: %r" % names[:1])
        wanted = ["Megler/megler.log", "Megler/megler.prev.log", "Megler/megler.prev2.log", "Megler/install-refusal.txt",
                  "Megler/flags.txt", "Megler/package.txt", "shared/settings.ini", "shared/apps/one.app/settings.ini"]
        for app in ("MeglerApp.one.app", "MeglerApp.two.app"):
            wanted += [app + "/megler.log", app + "/megler.prev.log", app + "/package.txt"]
        for name in wanted:
            check(name in names, "no entry " + name)
        check(not any(n.endswith(".part") for n in names), "a file half written (.part) is in the ZIP")
        check(len(names) == len(set(names)), "an entry is in the ZIP twice")
        check(len(names) == 1 + len(wanted), "the ZIP has %d entries, not %d: %r" % (len(names), 1 + len(wanted), names))
        if "Megler/megler.log" in names:
            check(z.read("Megler/megler.log") == (local / "megler.log").read_bytes(), "Megler's log is not its LocalState's")
        if "Megler/flags.txt" in names:
            check(z.read("Megler/flags.txt") == b"keep-screen-on\r\n", "flags.txt is %r" % z.read("Megler/flags.txt"))
        if "Megler/megler.prev2.log" in names:
            source = (local / "megler.prev2.log").read_bytes()
            kept = z.read("Megler/megler.prev2.log")
            note, _, rest = kept.partition(b"\r\n")
            check(note.startswith(b"-- megler: the first "), "the log over 16 MB does not begin with the note: %r" % note[:60])
            start = int(note.split(b" ")[4]) if note.startswith(b"-- megler: the first ") else -1
            check(start > len(source) - CAP and source[start - 1:start] == b"\n", "the note's start is not a line's within 16 MB of the end")
            check(rest == source[start:], "the log over 16 MB is not its last part from the note's start")
            check(len(rest) <= CAP, "the log over 16 MB keeps more than 16 MB")
        summary = z.read("summary.txt").decode("utf-8")
        for line in ("Megler: 0.1.0.999", "Core: 0.1.0 (the test's)", "Android Code: Can run", "ANGLE: In the package",
                     "Device family: Windows.Xbox", "MeglerApp.a.appx was not installed", "Megler (this package, from its LocalState)",
                     "MeglerApp.one.app", "shared/apps/one.app"):
            check(line in summary, "summary.txt does not say " + repr(line))
        print("zipfile: %d entries, %d bytes; summary.txt:\n%s" % (len(names), archive.stat().st_size, summary))

    for failure in failures:
        print("FAILED: " + failure)
    print("log-archive-check: %s" % ("passed" if not failures else "%d failures" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
