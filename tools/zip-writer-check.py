"""Reads the ZIP tools/zip-writer-test.cpp wrote with the shell's ZIP writer, as two readers of it do, and compares.

    python tools/zip-writer-check.py <folder>     (the folder zip-writer-test was given)

1. Python's zipfile: every entry's CRC-32 checked (testzip), the names in order, each entry's size, CRC-32, UTF-8 flag,
   time (DOS's two seconds) and bytes, and the ZIP's comment.
2. Explorer's ZIP folders (the Shell's zipfldr, through Shell.Application in PowerShell): every entry copied out of the ZIP
   as Explorer's Extract does, and compared with what was written.
Exits 0 when both read it right; says each difference otherwise.
"""
import datetime
import pathlib
import shutil
import subprocess
import sys
import time
import zipfile


def main():
    folder = pathlib.Path(sys.argv[1]).resolve()
    archive = folder / "test.zip"
    expected = []
    for line in (folder / "expected.txt").read_text(encoding="utf-8").splitlines():
        if line:
            name, size, crc, when = line.split("\t")
            expected.append((name, int(size), int(crc), when))
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # 1. Python's zipfile.
    with zipfile.ZipFile(archive) as z:
        check(z.testzip() is None, "zipfile: an entry's CRC-32 does not match its bytes")
        check(z.namelist() == [e[0] for e in expected], "zipfile: the names are %r" % z.namelist())
        check(z.comment == b"Megler's logs: a test of the shell's ZIP writer", "zipfile: the comment is %r" % z.comment)
        for index, (name, size, crc, when) in enumerate(expected):
            try:
                info = z.getinfo(name)
            except KeyError:
                failures.append("zipfile: no entry %s" % name)
                continue
            data = z.read(name)
            original = (folder / "expected" / ("%d.bin" % index)).read_bytes()
            check(data == original, "zipfile: %s's bytes differ" % name)
            check(info.file_size == size, "zipfile: %s's size is %d, not %d" % (name, info.file_size, size))
            check(info.CRC == crc, "zipfile: %s's CRC-32 is %08x, not %08x" % (name, info.CRC, crc))
            check(info.flag_bits & 0x800, "zipfile: %s is not marked UTF-8" % name)
            check(info.compress_type == zipfile.ZIP_DEFLATED, "zipfile: %s is not deflated" % name)
            stamp = datetime.datetime(*info.date_time)
            wanted = datetime.datetime.strptime(when, "%Y-%m-%d %H:%M:%S")
            check(abs((stamp - wanted).total_seconds()) <= 2, "zipfile: %s's time is %s, not %s" % (name, stamp, wanted))
        print("zipfile: %d entries read, %d bytes" % (len(z.namelist()), archive.stat().st_size))

    # 2. Explorer's ZIP folders: Extract, as the Shell's copy out of the ZIP's namespace does it. (It copies asynchronously:
    # waited for until every file is there whole.)
    out = folder / "explorer"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    script = (
        "$ErrorActionPreference = 'Stop'; $s = New-Object -ComObject Shell.Application; "
        "$z = $s.NameSpace('%s'); $d = $s.NameSpace('%s'); $d.CopyHere($z.Items(), 0x614)" % (archive, out)
    )
    subprocess.run(["powershell.exe", "-NoProfile", "-STA", "-Command", script], check=True)
    deadline = time.time() + 120
    while time.time() < deadline:
        if all((out / name).is_file() and (out / name).stat().st_size == size for name, size, _, _ in expected):
            break
        time.sleep(1)
    time.sleep(1)
    for index, (name, size, _, _) in enumerate(expected):
        path = out / name
        if not path.is_file():
            failures.append("Explorer: %s was not extracted" % name)
            continue
        original = (folder / "expected" / ("%d.bin" % index)).read_bytes()
        check(path.read_bytes() == original, "Explorer: %s's bytes differ" % name)
    print("Explorer: %d entries extracted" % sum(1 for name, _, _, _ in expected if (out / name).is_file()))

    for failure in failures:
        print("FAILED: " + failure)
    print("zip-writer-check: %s" % ("passed" if not failures else "%d failures" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
