r"""Reads the trace of the graphics kernel's calls that the flag kmt-trace writes (core/src/host/mg_kmt_trace.c):
LocalState\kmt-trace.bin of an app's package, on Windows 10 Mobile's ARM32.

    python tools/kmt-trace.py TRACE                     what was called, by whom, with what private data
    python tools/kmt-trace.py TRACE --ibs N [--context C] [--caller-module NAME]
                                                        and the first N command buffers submitted, as PM4 packets with
                                                        a5xx's register names (Mesa's freedreno XML: --registers DIR)
    python tools/kmt-trace.py TRACE --raw-ib K          the K-th submitted buffer's dwords alone, in hex

The register database is Mesa's src/freedreno/registers/adreno (a5xx.xml, adreno_pm4.xml), from a Mesa clone
(MEGLER_MESA_WORK\third_party\mesa, build\mesa by default; --registers names another).
"""
import argparse
import collections
import os
import struct
import sys
import xml.etree.ElementTree as ET

BLOB = {1: "private", 2: "private-before", 3: "alloc", 4: "runtime", 5: "ib1", 6: "ib2", 7: "handles", 8: "ops", 9: "note"}


def read_trace(path):
    data = open(path, "rb").read()
    magic, version, count, freq = struct.unpack_from("<4I", data, 0)
    if magic != 0x524D544B:
        sys.exit(f"{path}: not a kmt-trace file")
    at = 16
    names = []
    for _ in range(count):
        names.append(data[at:at + 48].split(b"\0")[0].decode())
        at += 48
    records = []
    while at + 40 <= len(data):
        head = struct.unpack_from("<8I", data, at)
        if head[0] != 0x4345524B:
            print(f"(a broken record at {at}: the trace ends there)")
            break
        qpc = struct.unpack_from("<Q", data, at + 32)[0]
        at += 40
        fn, status, tid, caller, args_size, nblobs = head[1:7]
        args = data[at:at + args_size]
        at += args_size
        blobs = []
        for _ in range(nblobs):
            kind, size, lo, hi = struct.unpack_from("<4I", data, at)
            at += 16
            blobs.append((kind, lo | hi << 32, data[at:at + size]))
            at += size
        if at > len(data):
            print("(the last record is cut short)")
            break
        records.append(dict(fn=names[fn] if fn < len(names) else f"#{fn}", status=status, tid=tid, caller=caller,
                            args=args, blobs=blobs, t=qpc / freq if freq else 0.0))
    return names, records


def u32s(b):
    return struct.unpack_from(f"<{len(b) // 4}I", b, 0)


def hexdw(b, n=64):
    w = u32s(b[:n * 4 - (n * 4 - len(b)) % 4] if len(b) % 4 else b[:n * 4])
    return " ".join(f"{x:08x}" for x in w) + (" ..." if len(b) > n * 4 else "")


# ----------------------------------------------------------- the registers


class Registers:
    def __init__(self, folder):
        self.regs = {}
        self.ops = {}
        self.fields = {}
        if not folder or not os.path.isdir(folder):
            return
        self._load(os.path.join(folder, "a5xx.xml"), "A5XX")
        self._load_ops(os.path.join(folder, "adreno_pm4.xml"))

    @staticmethod
    def _tag(e):
        return e.tag.split("}")[-1]

    def _load(self, path, domain):
        root = ET.parse(path).getroot()
        for d in root.iter():
            if self._tag(d) == "domain" and d.get("name") == domain:
                self._walk(d, 0, "")

    def _walk(self, node, base, prefix):
        for e in node:
            t = self._tag(e)
            if t in ("reg32", "reg64"):
                off = base + int(e.get("offset"), 0)
                name = prefix + e.get("name")
                self.regs.setdefault(off, name)
                if t == "reg64":
                    self.regs.setdefault(off + 1, name + "_HI")
            elif t == "array":
                off = base + int(e.get("offset"), 0)
                stride = int(e.get("stride", "1"), 0)
                for i in range(int(e.get("length", "1"), 0)):
                    for r in e:
                        if self._tag(r) in ("reg32", "reg64"):
                            o = off + i * stride + int(r.get("offset"), 0)
                            n = f"{e.get('name')}[{i}]" + ("" if r.get("name") in ("REG", None) else "." + r.get("name"))
                            self.regs.setdefault(o, n)
                            if self._tag(r) == "reg64":
                                self.regs.setdefault(o + 1, n + "_HI")

    @staticmethod
    def _a5xx(var):
        """Whether a value's variants ("A5XX", "A5XX-", "A2XX-A4XX", ...) take in the a5xx."""
        if not var:
            return True
        lo, dash, hi = var.partition("-")
        if not lo.startswith("A") or not lo.endswith("XX"):
            return True
        g = lambda x: int(x[1:-2])
        if not dash:
            return g(lo) == 5
        return g(lo) <= 5 and (not hi or 5 <= g(hi))

    def _load_ops(self, path):
        root = ET.parse(path).getroot()
        for e in root.iter():
            if self._tag(e) == "enum" and e.get("name") == "adreno_pm4_type3_packets":
                for v in e:
                    if self._tag(v) == "value" and self._a5xx(v.get("variants")):
                        self.ops.setdefault(int(v.get("value"), 0), v.get("name"))

    def reg(self, off):
        return self.regs.get(off, f"REG_0x{off:04x}")

    def op(self, op):
        return self.ops.get(op, f"OP_0x{op:02x}")


def decode(ib, regs, indent="  ", max_lines=4000):
    w = u32s(ib)
    out = []
    i = 0
    while i < len(w) and len(out) < max_lines:
        h = w[i]
        kind = h >> 28
        if kind == 4:
            cnt = h & 0x7f
            reg = (h >> 8) & 0x3ffff
            vals = w[i + 1:i + 1 + cnt]
            for k, v in enumerate(vals):
                out.append(f"{indent}{i + 1 + k:5d}  {regs.reg(reg + k)} = 0x{v:08x}")
            i += 1 + cnt
        elif kind == 7:
            cnt = h & 0x3fff
            op = (h >> 16) & 0x7f
            vals = w[i + 1:i + 1 + cnt]
            out.append(f"{indent}{i:5d}  {regs.op(op)} ({cnt}): " + " ".join(f"{v:08x}" for v in vals[:24]) +
                       (" ..." if cnt > 24 else ""))
            i += 1 + cnt
        else:
            out.append(f"{indent}{i:5d}  ?? 0x{h:08x}")
            i += 1
    return out


# ------------------------------------------------------------- the calls


def describe(r):
    a = r["args"]
    fn = r["fn"]
    st = r["status"]
    s = ""
    try:
        if fn == "CreateContextVirtual":
            dev, node, aff, flags, _, psize, hint, ctx = struct.unpack_from("<8I", a)
            s = f"device 0x{dev:x} node {node} affinity 0x{aff:x} flags 0x{flags:x} hint {hint} -> context 0x{ctx:x}, private {psize} B"
        elif fn == "CreateDevice":
            adapter, flags, dev = struct.unpack_from("<3I", a)
            s = f"adapter 0x{adapter:x} flags 0x{flags:x} -> device 0x{dev:x}"
        elif fn == "SubmitCommand":
            va, length, flags = struct.unpack_from("<QII", a)
            count = struct.unpack_from("<I", a, 24)[0]
            ctx = struct.unpack_from("<I", a, 28)[0]
            psize = struct.unpack_from("<I", a, 28 + 256 + 4)[0]
            s = f"context 0x{ctx:x} (of {count}) commands 0x{va:x} {length} B flags 0x{flags:x} private {psize} B"
        elif fn == "Escape":
            adapter, dev, typ, flags, _, size, ctx = struct.unpack_from("<7I", a)
            s = f"adapter 0x{adapter:x} device 0x{dev:x} context 0x{ctx:x} type {typ} flags 0x{flags:x} size {size}"
        elif fn == "QueryAdapterInfo":
            adapter, typ, _, size = struct.unpack_from("<4I", a)
            s = f"adapter 0x{adapter:x} type {typ} size {size}"
        elif fn == "MapGpuVirtualAddress":
            alloc = struct.unpack_from("<I", a, 32)[0]
            off, pages = struct.unpack_from("<QQ", a, 40)
            va = struct.unpack_from("<Q", a, 88)[0]
            s = f"allocation 0x{alloc:x} pages {off}+{pages} -> 0x{va:x}"
        elif fn == "Lock2":
            dev, alloc, flags, p = struct.unpack_from("<4I", a)
            s = f"allocation 0x{alloc:x} flags 0x{flags:x} -> 0x{p:x}"
        elif fn == "CreatePagingQueue":
            dev, prio, q, sync = struct.unpack_from("<4I", a)
            s = f"device 0x{dev:x} priority {prio} -> queue 0x{q:x} sync 0x{sync:x}"
        elif fn == "CreateAllocation":
            dev, res, share, _, rts, _, pds, n = struct.unpack_from("<8I", a)
            s = f"device 0x{dev:x} resource 0x{res:x} allocations {n} runtime {rts} B private {pds} B"
        elif fn in ("SetContextSchedulingPriority", "SetContextInProcessSchedulingPriority"):
            ctx, prio = struct.unpack_from("<Ii", a)
            s = f"context 0x{ctx:x} priority {prio}"
    except struct.error:
        pass
    return f"{fn} {s} -> 0x{st:08x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--ibs", type=int, default=0, help="decode the first N submitted command buffers")
    ap.add_argument("--skip", type=int, default=0, help="of the submitted buffers, skip the first K")
    ap.add_argument("--context", help="only this context's submits (hex)")
    ap.add_argument("--raw-ib", type=int, help="the K-th submitted buffer in hex")
    ap.add_argument("--registers", help="Mesa's src/freedreno/registers/adreno")
    ap.add_argument("--all", action="store_true", help="every call, not only the telling ones")
    a = ap.parse_args()
    reg_dir = a.registers or os.path.join(os.environ.get("MEGLER_MESA_WORK", os.path.join(os.path.dirname(__file__), "..",
                                          "build", "mesa")), "third_party", "mesa", "src", "freedreno", "registers", "adreno")
    names, records = read_trace(a.trace)
    regs = Registers(reg_dir)
    if not regs.regs:
        print(f"(no register database in {reg_dir}: registers by number)")
    counts = collections.Counter(r["fn"] for r in records)
    print(f"{len(records)} calls: " + ", ".join(f"{k} {v}" for k, v in counts.most_common()))
    callers = collections.Counter((r["fn"], r["caller"] & 0xfff00000) for r in records)
    print("callers (by 1 MB region): " + ", ".join(f"{fn}@{c:08x} {n}" for (fn, c), n in sorted(callers.items())[:40]))
    telling = {"CreateDevice", "CreateContext", "CreateContextVirtual", "CreatePagingQueue", "Escape", "QueryAdapterInfo",
               "SetContextSchedulingPriority", "SetContextInProcessSchedulingPriority", "Render"}
    submits = []
    for r in records:
        if r["fn"] == "SubmitCommand":
            submits.append(r)
        if not a.all and r["fn"] not in telling:
            continue
        print(f"{r['t']:12.6f} tid {r['tid']:5d} from {r['caller']:08x}  {describe(r)}")
        for kind, addr, blob in r["blobs"]:
            if kind in (1, 2, 3, 4, 9):
                text = blob.decode("ascii", "replace") if kind == 9 else hexdw(blob, 96)
                print(f"      {BLOB.get(kind, kind)} [{addr:x}] {len(blob)} B: {text}")
    by_ctx = collections.Counter()
    for r in submits:
        by_ctx[struct.unpack_from("<I", r["args"], 28)[0]] += 1
    print("submits by context: " + ", ".join(f"0x{c:x} {n}" for c, n in by_ctx.items()))
    privs = collections.Counter(next((blob for kind, _, blob in r["blobs"] if kind == 1), b"").hex() for r in submits)
    print("submit private data seen: " + "; ".join(f"[{len(bytes.fromhex(p))} B] {p[:160]} x{n}" for p, n in privs.most_common(8)))
    if a.context:
        submits = [r for r in submits if struct.unpack_from("<I", r["args"], 28)[0] == int(a.context, 16)]
    if a.raw_ib is not None:
        r = submits[a.raw_ib]
        for kind, addr, blob in r["blobs"]:
            if kind == 5:
                print(hexdw(blob, len(blob) // 4))
        return
    for k, r in enumerate(submits[a.skip:a.skip + a.ibs]):
        print(f"\n== submit {a.skip + k} at {r['t']:.6f}: {describe(r)}")
        for kind, addr, blob in r["blobs"]:
            if kind == 1:
                print(f"   private {len(blob)} B: {hexdw(blob, 64)}")
            elif kind == 9:
                print(f"   ({blob.decode('ascii', 'replace')})")
            elif kind in (5, 6):
                print(f"   {BLOB[kind]} 0x{addr:x}, {len(blob) // 4} dwords:")
                for line in decode(blob, regs):
                    print(line)


if __name__ == "__main__":
    main()
