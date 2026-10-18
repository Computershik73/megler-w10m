r"""Names what the flag trace-profile sampled: LocalState\profile.txt, by function.

    python tools/profile-names.py [PROFILE] [--base EARLIER] [--version N] [--map MODULE=FILE] [--lib FILE.so] [--top N]

PROFILE is a profile.txt pulled from the phone; without it the phone's is
fetched (tools/phone.py's Device Portal). Each sample is an address in a
module (module, offset); the offsets are put to functions with:

  Megler.exe       build/maps/Megler_<version>.map (--version), else the last
                 build's build/appx-arm/link/Megler.map
  libGLESv2.dll  build/angle/maps/... when Megler's own ANGLE is built
  libEGL.dll     (build/angle/dist), else gecko-w10m's (C:/rw-obj)
  FILE.so        an app's library (--lib, the one the APK has): its
                 exported symbols, which are often far from the code
                 between them -- such a name is said with its distance
  MODULE=FILE    any other MSVC/lld-link map (--map)

The system's DLLs and the driver have no names here: they are said by module.
The file counts from the profiler's start; --base, a profile.txt fetched
earlier in the same run, is taken away from it: what came between the two
(a race, without the menus before it).
For each thread: where it ran, by function, and what it waited for (the
first return address outside the system and the layer when it was stopped
in a system call), and the ANGLE function each sample was under (its pc
in libGLESv2.dll, or the first return address into it): the time spent in
and below each of ANGLE's functions -- Direct3D, the driver, the kernel --
the most first.
"""
import argparse
import bisect
import collections
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def demangle(name):
    """MSVC's ?name@scope@@... as scope::name (enough to read; templates are left as they are)."""
    if not name.startswith("?") or name.startswith("??"):
        return name
    body = name[1:].split("@@", 1)[0]
    parts = [p for p in body.split("@") if p]
    return "::".join(reversed(parts)) if parts else name


def read_map(path):
    """An MSVC or lld-link map: (sorted addresses relative to the image, names)."""
    base = None
    syms = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if base is None:
                m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
                if m:
                    base = int(m.group(1), 16)
                    continue
            m = re.match(r"\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s", line)
            if m and int(m.group(1), 16) != 0:
                syms.append((int(m.group(3), 16), m.group(2)))
    base = base or 0
    syms = sorted((a - base, n) for a, n in syms if a >= base)
    return [a for a, _ in syms], [demangle(n) for _, n in syms]


def read_elf_dynsym(path):
    """A 32-bit ELF's defined dynamic symbols that are functions: (sorted addresses, names)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("%s: not a 32-bit ELF" % path)
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    syms = []
    for sh in sections:
        if sh[1] != 11:  # SHT_DYNSYM
            continue
        strtab = sections[sh[6]]
        for k in range(sh[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, sh[4] + k * 16)
            if not shndx or (info & 0xF) != 2:  # defined, STT_FUNC
                continue
            end = data.index(b"\0", strtab[4] + name)
            syms.append((value & ~1, data[strtab[4] + name:end].decode("utf-8", "replace")))
    syms.sort()
    return [a for a, _ in syms], demangle_itanium([n for _, n in syms])


def demangle_itanium(names):
    """C++ names of an ELF's (_Z...) as source has them, by llvm-cxxfilt where it is; else as they are."""
    import shutil
    import subprocess
    tool = shutil.which("llvm-cxxfilt") or next((p for p in ("C:/Program Files/LLVM/bin/llvm-cxxfilt.exe",) if os.path.exists(p)), None)
    if not tool:
        return names
    r = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    return out if r.returncode == 0 and len(out) == len(names) else names


def default_maps(version):
    maps = {}
    megler = os.path.join(ROOT, "build", "maps", "Megler_0.1.0.%s.map" % version) if version else None
    maps["Megler.exe"] = megler if megler and os.path.exists(megler) else os.path.join(ROOT, "build", "appx-arm", "link", "Megler.map")
    own = os.path.isdir(os.path.join(ROOT, "build", "angle", "dist")) and not os.environ.get("MEGLER_ANGLE")
    for name, sub in (("libGLESv2.dll", "libGLESv2_gn/libGLESv2.map"), ("libEGL.dll", "libEGL_gn/libEGL.map")):
        path = os.path.join(ROOT, "build", "angle", "maps", sub) if own else os.path.join("C:/rw-obj/third_party/angle", sub)
        maps[name] = path
    return maps


def entry(line):
    """A line "run|wait|under MODULE 0xOFFSET COUNT" as its four parts; None for any other (or one cut short)."""
    parts = line.split(" ")
    if len(parts) != 4 or parts[0] not in ("run", "wait", "under") or not parts[3].isdigit():
        return None
    return parts[0], parts[1], parts[2], int(parts[3])


def parse_counts(text):
    """{(thread line's tid, kind, module, offset): count} and {tid: [samples, waits, unknown]}."""
    counts, heads, tid = {}, {}, None
    for line in text.splitlines():
        m = re.match(r'thread (\d+) "([^"]*)" samples (\d+) waits (\d+) unknown (\d+)', line)
        e = entry(line)
        if m:
            tid = m.group(1)
            heads[tid] = [m.group(2)] + [int(x) for x in m.groups()[2:]]
        elif tid and e:
            counts[(tid,) + e[:3]] = e[3]
    return counts, heads


def subtract(text, base):
    """The profile `text` less `base` (an earlier one of the same run), as text of the same form."""
    now, heads = parse_counts(text)
    then, then_heads = parse_counts(base)
    out = [text.splitlines()[0] + " (less an earlier profile)"] if text.startswith("#") else []
    for tid, (name, samples, waits, unknown) in heads.items():
        s0 = then_heads.get(tid, [name, 0, 0, 0])
        out.append('thread %s "%s" samples %d waits %d unknown %d' % (tid, name, max(0, samples - s0[1]), max(0, waits - s0[2]), max(0, unknown - s0[3])))
        for (t, kind, module, off), c in now.items():
            d = c - then.get((t, kind, module, off), 0)
            if t == tid and d > 0:
                out.append("%s %s %s %d" % (kind, module, off, d))
    return "\n".join(out) + "\n"


def fetch_profile():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import phone
    data = phone.read_app_file(phone.package()["PackageFullName"], "profile.txt")
    if data is None:
        raise SystemExit("no LocalState\\profile.txt on the phone (was the flag trace-profile on, and ten seconds gone?)")
    return data.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", nargs="?")
    ap.add_argument("--base", help="an earlier profile.txt of the same run, taken away")
    ap.add_argument("--version", help="the package's build number, for its Megler.exe map")
    ap.add_argument("--map", action="append", default=[], help="MODULE=FILE")
    ap.add_argument("--lib", action="append", default=[], help="an app's .so, named as the module of its file name")
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()

    text = open(a.profile, encoding="utf-8", errors="replace").read() if a.profile else fetch_profile()
    if a.base:
        text = subtract(text, open(a.base, encoding="utf-8", errors="replace").read())
    tables = {}
    for module, path in default_maps(a.version).items():
        if os.path.exists(path):
            tables[module.lower()] = read_map(path)
    for item in a.map:
        module, _, path = item.partition("=")
        tables[module.lower()] = read_map(path)
    for path in a.lib:
        tables[os.path.basename(path).lower()] = read_elf_dynsym(path)
    print("names from: " + (", ".join(sorted(tables)) or "nothing (every address is said by module)"))

    def name_of(module, off):
        t = tables.get(module.lower())
        if not t:
            return module
        i = bisect.bisect_right(t[0], off) - 1
        if i < 0:
            return module
        far = off - t[0][i]
        return "%s %s%s" % (module, t[1][i], " (+0x%x)" % far if module.lower().endswith(".so") and far > 0x4000 else "")

    threads = []
    for line in text.splitlines():
        if line.startswith("thread "):
            m = re.match(r'thread (\d+) "([^"]*)" samples (\d+) waits (\d+) unknown (\d+)', line)
            if not m:
                continue
            threads.append({"head": m.groups(), "run": collections.Counter(), "wait": collections.Counter(), "under": collections.Counter(),
                            "rmod": collections.Counter(), "wmod": collections.Counter(), "umod": collections.Counter()})
        elif threads and entry(line):
            kind, module, off, count = entry(line)
            t = threads[-1]
            t[kind][name_of(module, int(off, 16))] += count
            t[kind[0] + "mod"][module] += count
    if text.startswith("#"):
        print(text.splitlines()[0])
    for t in threads:
        tid, name, samples, waits, unknown = t["head"]
        samples, waits, unknown = int(samples), int(waits), int(unknown)
        if not samples:
            print("\n== thread %s \"%s\": no samples" % (tid, name))
            continue
        print("\n== thread %s \"%s\": %d samples, %d running, %d waiting (%d of them for a caller not found)" % (tid, name, samples, samples - waits, waits, unknown))
        print("   running, by module: " + ", ".join("%s %.1f%%" % (m, 100.0 * c / samples) for m, c in t["rmod"].most_common(12)))
        for fn, c in t["run"].most_common(a.top):
            print("   %5.1f%%  %s" % (100.0 * c / samples, fn))
        print("   waiting for, by module: " + ", ".join("%s %.1f%%" % (m, 100.0 * c / samples) for m, c in t["wmod"].most_common(12)))
        for fn, c in t["wait"].most_common(max(10, a.top // 2)):
            print("   %5.1f%%  %s" % (100.0 * c / samples, fn))
        if t["under"]:
            print("   under ANGLE's functions (with all below them), %.1f%% of its samples:" % (100.0 * sum(t["under"].values()) / samples))
            for fn, c in t["under"].most_common(a.top):
                print("   %5.1f%%  %s" % (100.0 * c / samples, fn))


if __name__ == "__main__":
    main()
