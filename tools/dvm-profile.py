"""Where a benchmark item's time goes, from the samples hk_dvm_bench.c wrote (tools/dvm-bench.sh profile).

    python tools/dvm-profile.py <samples> <hk_tests.map> <hk_tests.exe> [function [lines]]

The samples are addresses where the running thread was found; the linker map names the function each is in. With a
function named, its hottest instructions are shown in their surroundings, from dumpbin's disassembly, with the share of
samples each instruction had.
"""
import bisect
import os
import re
import subprocess
import sys


def read_map(path):
    syms = []
    in_table = False
    for line in open(path, encoding="latin-1"):
        if "Publics by Value" in line or line.startswith(" Static symbols"):
            in_table = True
            continue
        if not in_table:
            continue
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return syms


def main():
    samples, mapfile, exe = sys.argv[1:4]
    func = sys.argv[4] if len(sys.argv) > 4 else None
    nlines = int(sys.argv[5]) if len(sys.argv) > 5 else 40
    lines = open(samples).read().split()
    anchor = int(lines[1], 16)
    pcs = [int(x, 16) for x in lines[2:]]
    syms = read_map(mapfile)
    addrs = [a for a, _ in syms]
    anchor_va = next(a for a, n in syms if n.endswith("bench_run"))
    delta = anchor - anchor_va
    counts = {}
    hot = {}
    for pc in pcs:
        va = pc - delta
        i = bisect.bisect_right(addrs, va) - 1
        name = syms[i][1] if i >= 0 else "?"
        # Far past the nearest name: not the program's code (a system DLL: the clock, a lock, the kernel's entry).
        if i < 0 or va - syms[i][0] > 0x10000:
            name = "(outside the program: system DLLs)"
        counts[name] = counts.get(name, 0) + 1
        if func and name == func:
            hot[va] = hot.get(va, 0) + 1
    total = len(pcs)
    print("%d samples" % total)
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1])[:30]:
        print("%6.1f%%  %s" % (100.0 * c / total, name))
    if not func or not hot:
        return
    vc = os.environ.get("VCBIN", "")
    dumpbin = os.path.join(vc, "dumpbin.exe") if vc else "dumpbin.exe"
    start = next(a for a, n in syms if n == func)
    i = addrs.index(start)
    end = addrs[i + 1] if i + 1 < len(addrs) else start + 0x10000
    out = subprocess.run([dumpbin, "/nologo", "/disasm:nobytes", "/range:0x%x,0x%x" % (start, end), exe], capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9A-F]{16}):\s+(.*)", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip()))
    fsum = sum(hot.values())
    print("\n%s: %d samples, %.1f%% of all" % (func, fsum, 100.0 * fsum / total))
    for va, insn in insns:
        c = hot.get(va, 0)
        if c * 1000 >= fsum * 3 or nlines < 0:
            print("%6.1f%%  %x  %s" % (100.0 * c / fsum, va - start, insn))
    if nlines > 0:
        # the hottest stretches, each with the instructions around it
        ranked = sorted(hot.items(), key=lambda kv: -kv[1])[:nlines // 8 or 1]
        shown = set()
        for va, _ in ranked:
            k = next((j for j, (a, _) in enumerate(insns) if a == va), None)
            if k is None:
                continue
            print("--")
            for j in range(max(0, k - 6), min(len(insns), k + 4)):
                if j in shown:
                    continue
                shown.add(j)
                a, t = insns[j]
                print("%6.1f%%  %x  %s" % (100.0 * hot.get(a, 0) / fsum, a - start, t))


if __name__ == "__main__":
    main()
