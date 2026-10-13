r"""Take the DEX files out of Android 9's .vdex files.

    python tools/vdex-dex.py build/android9/system/framework build/android9/boot [NAME...]

An image whose boot class path was compiled ahead of time keeps no classes.dex
in its jars; the bytecode is in /system/framework/boot*.vdex, one file to a jar
(boot.vdex is core-oj.jar's). NAMEs pick jars ("core-oj", "framework"); none
means all. Each DEX file found is written as <jar>.dex, or <jar>-classesN.dex
for the ones after a jar's first.

A .vdex of Android 9 (version 019) holds either the jar's DEX files as they
were, or -- what the image this was written for has -- **compact DEX**
("cdex001"), ART's own form of the same thing, which nothing but ART reads.
This turns compact DEX back into DEX. What differs, and is undone:

  - every offset into the data section counts from the start of that section,
    not of the file, and the section is one for all of a jar's DEX files (each
    file written here gets a copy of it);
  - a method's code has a packed header: four sizes in four bits each, the
    instruction count in eleven, and 16-bit words *before* the item for what
    does not fit ("preheader"); the registers are counted without the
    arguments;
  - where a method's debug information is, is not in its code but in a table
    by method index (sixteen methods to a block, a bit for each that has any,
    LEB128 steps from one to the next).

Two more things are Android 9's and not compact DEX's, and are undone as well,
so that what comes out is what javac and dx made:

  - its non-public API is marked in these files by bending access flags (all
    three visibility bits flipped; 0x20 on a field or a method that is not
    native; 0x200 on a native method);
  - "quickened" code. In methods it did not compile to machine code, the
    device's compiler writes over instructions with forms only ART's
    interpreter knows: a field access by the field's offset in the object
    (0xe3 to 0xe8, 0xeb to 0xf2), a virtual call by the method's place in the
    vtable (0xe9, 0xea), a check-cast it proved needless as two nops, and
    return-void as 0x73. What stood there before is kept at the end of the
    .vdex: for each method a list of 16-bit indexes, one for each such
    instruction in order (two for a check-cast: the register and the type; a
    real nop has 0xffff). ART's own unquickening is followed step by step.

The file written keeps the old data section whole and adds the rebuilt code and
class data after it, so it has stretches nothing points at, and a map that
names only the index sections and the code. That is enough for this project's
reader and for d8, which writes a tidy file from it (tools/android9-boot.sh
does that); ART's own verifier would refuse it.

Checked as it goes: every try block lies inside its method's code, every
handler offset inside the handler list, every index within its table.
"""
import hashlib
import os
import struct
import sys
import zlib


def uleb(d, p):
    v = shift = 0
    while True:
        b = d[p]
        p += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return v, p


def sleb(d, p):
    v = shift = 0
    while True:
        b = d[p]
        p += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            if b & 0x40:
                v -= 1 << shift
            return v, p


def put_uleb(out, v):
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


# Code units to an instruction, by opcode (Android 9's set, ART's own opcodes included).
LENGTH = [1] * 256
for lo, hi, units in ((0x02, 0x02, 2), (0x03, 0x03, 3), (0x05, 0x05, 2), (0x06, 0x06, 3), (0x08, 0x08, 2), (0x09, 0x09, 3), (0x13, 0x13, 2),
                      (0x14, 0x14, 3), (0x15, 0x16, 2), (0x17, 0x17, 3), (0x18, 0x18, 5), (0x19, 0x1A, 2), (0x1B, 0x1B, 3), (0x1C, 0x1C, 2),
                      (0x1F, 0x20, 2), (0x22, 0x23, 2), (0x24, 0x26, 3), (0x29, 0x29, 2), (0x2A, 0x2C, 3), (0x2D, 0x3D, 2), (0x44, 0x6D, 2),
                      (0x6E, 0x72, 3), (0x74, 0x78, 3), (0x90, 0xAF, 2), (0xD0, 0xE2, 2), (0xE3, 0xE8, 2), (0xE9, 0xEA, 3), (0xEB, 0xF2, 2),
                      (0xFA, 0xFB, 4), (0xFC, 0xFD, 3), (0xFE, 0xFF, 2)):
    for op in range(lo, hi + 1):
        LENGTH[op] = units

# What each quickened opcode stood for.
UNQUICK = {0xE3: 0x52, 0xE4: 0x53, 0xE5: 0x54, 0xE6: 0x59, 0xE7: 0x5A, 0xE8: 0x5B, 0xE9: 0x6E, 0xEA: 0x74, 0xEB: 0x5C, 0xEC: 0x5D, 0xED: 0x5E,
           0xEE: 0x5F, 0xEF: 0x55, 0xF0: 0x56, 0xF1: 0x57, 0xF2: 0x58}


def unquicken(code, indexes, what):
    """`code` (a bytearray of code units) with ART's own instructions put back; `indexes` is the method's list, or None."""
    at = 0
    used = 0
    count = len(indexes) if indexes else 0
    units = len(code) // 2
    changed = 0
    while at < units:
        unit = code[at * 2] | (code[at * 2 + 1] << 8)
        op = unit & 0xFF
        size = LENGTH[op]
        if op == 0:
            if unit == 0x0100:
                size = 4 + (code[at * 2 + 2] | (code[at * 2 + 3] << 8)) * 2
            elif unit == 0x0200:
                size = 2 + (code[at * 2 + 2] | (code[at * 2 + 3] << 8)) * 4
            elif unit == 0x0300:
                width = code[at * 2 + 2] | (code[at * 2 + 3] << 8)
                n = int.from_bytes(code[at * 2 + 4:at * 2 + 8], "little")
                size = 4 + (width * n + 1) // 2
            if count:
                # Every nop has an entry, a switch table or array data (whose first byte is a nop's) included.
                assert used < count, "%s: the list of what was quickened is too short" % what
                first = indexes[used]
                used += 1
                if first != 0xFFFF:
                    assert unit == 0 and used < count and at + 1 < units and code[at * 2 + 2] == 0 and code[at * 2 + 3] == 0, \
                        "%s: a check-cast is to be put back where there is no room for one" % what
                    type_idx = indexes[used]
                    used += 1
                    assert first < 256
                    code[at * 2] = 0x1F
                    code[at * 2 + 1] = first
                    code[at * 2 + 2] = type_idx & 0xFF
                    code[at * 2 + 3] = type_idx >> 8
                    size = 2
                    changed += 1
        elif op == 0x73:
            code[at * 2] = 0x0E
            changed += 1
        elif op in UNQUICK:
            assert used < count, "%s: a quickened instruction with nothing to say what it was" % what
            index = indexes[used]
            used += 1
            code[at * 2] = UNQUICK[op]
            code[at * 2 + 2] = index & 0xFF
            code[at * 2 + 3] = index >> 8
            changed += 1
        at += size
    assert at == units, "%s: the last instruction runs past the code" % what
    assert used == count, "%s: %d of the %d entries of what was quickened were used" % (what, used, count)
    return changed


def plain_flags(flags, is_method):
    """Android 9's hidden-API marks out of a field's or method's access flags."""
    marked = 0
    vis = flags & 7
    if vis & (vis - 1):
        flags ^= 7
        marked = 1
    second = 0x200 if (is_method and flags & 0x100) else 0x20
    if flags & second:
        # A native method may really be synchronized (0x20); only its 0x200 is the mark.
        flags &= ~second
        marked = 1
    return flags, marked


class Compact:
    """One compact DEX file inside a .vdex."""

    def __init__(self, vdex, at, quick_at):
        self.v = vdex
        self.at = at
        # The quickening section of the .vdex (0: none), and in it this file's table, whose offset is the word before the file.
        self.quick_at = quick_at
        self.quick_table = struct.unpack_from("<I", vdex, at - 4)[0] if quick_at else 0
        self.quick_of_code = {}
        h = struct.unpack_from("<8sI20sIIIIIIIIIIIIIIIIIIII", vdex, at)
        (_, _, _, self.file_size, self.header_size, endian, _, _, self.map_off, self.strings_n, self.strings_off, self.types_n, self.types_off,
         self.protos_n, self.protos_off, self.fields_n, self.fields_off, self.methods_n, self.methods_off, self.classes_n, self.classes_off,
         self.data_size, self.data_off) = h
        assert endian == 0x12345678 and self.header_size >= 0x88, "not a compact DEX header"
        (self.features, self.debug_pos, self.debug_table, self.debug_base, self.owned_begin, self.owned_end) = struct.unpack_from("<6I", vdex, at + 0x70)
        self.base = at + self.data_off          # where data offsets count from, in the .vdex
        assert self.base + self.data_size <= len(vdex), "the data section runs past the file"

    def debug_info_off(self, method_idx):
        v = self.v
        data = self.base + self.debug_pos
        block = data + struct.unpack_from("<I", v, data + self.debug_table + 4 * (method_idx // 16))[0]
        mask = (v[block] << 8) | v[block + 1]
        block += 2
        bit = method_idx % 16
        if not (mask >> bit) & 1:
            return 0
        count = bin(mask & ((1 << (bit + 1)) - 1)).count("1")
        off = self.debug_base
        for _ in range(count):
            # A step back is written as the 32-bit number that wraps around to it.
            step, block = uleb(v, block)
            off = (off + step) & 0xFFFFFFFF
        return off

    def quickened(self, method_idx):
        """The indexes that say what a method's quickened instructions were (None: it has none)."""
        if not self.quick_at:
            return None
        v = self.v
        head = self.quick_at + self.quick_table
        minimum, table = struct.unpack_from("<II", v, head)
        data = head + 8
        block = data + struct.unpack_from("<I", v, data + table + 4 * (method_idx // 16))[0]
        mask = (v[block] << 8) | v[block + 1]
        block += 2
        bit = method_idx % 16
        if not (mask >> bit) & 1:
            return None
        count = bin(mask & ((1 << (bit + 1)) - 1)).count("1")
        off = minimum
        for _ in range(count):
            step, block = uleb(v, block)
            off = (off + step) & 0xFFFFFFFF
        if not off:
            return None
        # (One is added to every offset so that none is zero.)
        n, p = uleb(v, self.quick_at + off - 1)
        return list(struct.unpack_from("<%dH" % n, v, p))

    def code(self, code_off, debug_off, delta, method_idx, stats):
        """The code item at `code_off` of the data section, as DEX has it; `delta` is what a data offset gains."""
        v = self.v
        co = self.base + code_off
        fields, count_flags = struct.unpack_from("<HH", v, co)
        insns = count_flags >> 5
        flags = count_flags & 0x1F
        registers, ins, outs, tries = (fields >> 12) & 15, (fields >> 8) & 15, (fields >> 4) & 15, fields & 15
        pre = co
        if flags & 0x10:
            pre -= 2
            insns += struct.unpack_from("<H", v, pre)[0]
            pre -= 2
            insns += struct.unpack_from("<H", v, pre)[0] << 16
        if flags & 0x01:
            pre -= 2
            registers += struct.unpack_from("<H", v, pre)[0]
        if flags & 0x02:
            pre -= 2
            ins += struct.unpack_from("<H", v, pre)[0]
        if flags & 0x04:
            pre -= 2
            outs += struct.unpack_from("<H", v, pre)[0]
        if flags & 0x08:
            pre -= 2
            tries += struct.unpack_from("<H", v, pre)[0]
        registers += ins
        assert registers < 65536 and ins <= registers, "a code item's sizes make no sense"
        start = co + 4
        end = start + insns * 2
        assert end <= self.base + self.data_size, "code runs past the data section"
        out = bytearray(struct.pack("<HHHHII", registers, ins, outs, tries, debug_off + delta if debug_off else 0, insns))
        # Code shared by several methods is put back once, by the first method's list, as ART does it.
        if code_off not in self.quick_of_code:
            self.quick_of_code[code_off] = self.quickened(method_idx)
        body = bytearray(v[start:end])
        stats["unquickened"] += unquicken(body, self.quick_of_code[code_off], "method %d" % method_idx)
        out += body
        if tries:
            if insns & 1:
                out += b"\0\0"
            # ART finds the tries at the next address that is a multiple of four; the .vdex is mapped at a page's start.
            t = (end + 3) & ~3
            handlers = t + tries * 8
            p = handlers
            n, p = uleb(v, p)
            starts = set()
            for _ in range(n):
                starts.add(p - handlers)
                size, p = sleb(v, p)
                for _ in range(abs(size)):
                    type_idx, p = uleb(v, p)
                    addr, p = uleb(v, p)
                    assert type_idx < self.types_n and addr < insns, "a catch handler makes no sense"
                if size <= 0:
                    addr, p = uleb(v, p)
                    assert addr < insns, "a catch-all handler makes no sense"
            for i in range(tries):
                start_addr, count, handler_off = struct.unpack_from("<IHH", v, t + i * 8)
                assert start_addr + count <= insns and handler_off in starts, "a try block makes no sense"
            out += v[t:p]
        while len(out) & 3:
            out.append(0)
        return bytes(out)

    def convert(self):
        v = self.v
        ids_end = self.classes_off + self.classes_n * 32
        assert ids_end <= self.file_size
        # The new file: header, the index sections where they were less the longer header, then the data section.
        shift = 0x70 - self.header_size
        new_data_off = (ids_end + shift + 3) & ~3
        delta = new_data_off                      # a data offset d becomes new_data_off + d
        blob = bytearray(v[self.base:self.base + self.data_size])
        while len(blob) & 3:
            blob.append(0)
        ids = bytearray(v[self.at + self.header_size:self.at + ids_end])

        def ids_at(off):
            return off - self.header_size

        # String data and parameter lists: a data offset each.
        for i in range(self.strings_n):
            p = ids_at(self.strings_off) + i * 4
            struct.pack_into("<I", ids, p, struct.unpack_from("<I", ids, p)[0] + delta)
        for i in range(self.protos_n):
            p = ids_at(self.protos_off) + i * 12 + 8
            off = struct.unpack_from("<I", ids, p)[0]
            if off:
                struct.pack_into("<I", ids, p, off + delta)

        patched = set()

        def patch(off):
            """Add delta to the u32 at data offset `off` of the copied section, once, if it is not zero. Returns the old value."""
            old = struct.unpack_from("<I", blob, off)[0]
            if old and off not in patched:
                patched.add(off)
                struct.pack_into("<I", blob, off, old + delta)
            return old

        def patch_set(off):
            if not off or ("set", off) in patched:
                return
            patched.add(("set", off))
            n = struct.unpack_from("<I", blob, off)[0]
            for i in range(n):
                patch(off + 4 + i * 4)

        def patch_directory(off):
            if ("dir", off) in patched:
                return
            patched.add(("dir", off))
            class_set = patch(off)
            patch_set(class_set)
            nf, nm, np = struct.unpack_from("<III", blob, off + 4)
            p = off + 16
            for _ in range(nf + nm):
                patch_set(patch(p + 4))
                p += 8
            for _ in range(np):
                ref = patch(p + 4)
                p += 8
                if ref and ("ref", ref) not in patched:
                    patched.add(("ref", ref))
                    n = struct.unpack_from("<I", blob, ref)[0]
                    for i in range(n):
                        patch_set(patch(ref + 4 + i * 4))

        tail = bytearray()                        # what is added after the copied section
        tail_at = len(blob)
        codes = {}
        stats = {"code": 0, "shared": 0, "marked": 0, "methods": 0, "fields": 0, "unquickened": 0}

        def code_at(code_off, method_idx):
            # Compact DEX lets methods with the same code share one item. Each gets its own here: d8 1.5 counts shared code
            # once in the map it writes and writes it once for each method, which no other reader forgives.
            debug_off = self.debug_info_off(method_idx)
            if code_off in codes:
                stats["shared"] += 1
            codes[code_off] = True
            item = self.code(code_off, debug_off, delta, method_idx, stats)
            where = tail_at + len(tail)
            tail.extend(item)
            stats["code"] += 1
            return where

        class_data = {}
        for i in range(self.classes_n):
            p = ids_at(self.classes_off) + i * 32
            (class_idx, access, super_idx, interfaces_off, source_idx, annotations_off, data_off, static_off) = struct.unpack_from("<8I", ids, p)
            assert class_idx < self.types_n
            if interfaces_off:
                interfaces_off += delta
            if annotations_off:
                patch_directory(annotations_off)
                annotations_off += delta
            if static_off:
                static_off += delta
            if data_off:
                if data_off not in class_data:
                    q = self.base + data_off
                    counts = []
                    for _ in range(4):
                        c, q = uleb(v, q)
                        counts.append(c)
                    out = bytearray()
                    for c in counts:
                        put_uleb(out, c)
                    for part in range(2):
                        idx = 0
                        for _ in range(counts[part]):
                            diff, q = uleb(v, q)
                            flags, q = uleb(v, q)
                            idx += diff
                            assert idx < self.fields_n
                            flags, marked = plain_flags(flags, False)
                            stats["marked"] += marked
                            stats["fields"] += 1
                            put_uleb(out, diff)
                            put_uleb(out, flags)
                    methods = []
                    for part in range(2, 4):
                        idx = 0
                        for _ in range(counts[part]):
                            diff, q = uleb(v, q)
                            flags, q = uleb(v, q)
                            code_off, q = uleb(v, q)
                            idx += diff
                            assert idx < self.methods_n
                            flags, marked = plain_flags(flags, True)
                            stats["marked"] += marked
                            stats["methods"] += 1
                            methods.append((diff, flags, code_off, idx))
                    # The code first (it must not land in the middle of this class's own data), then the class data.
                    placed = [(diff, flags, code_at(code_off, idx) + delta if code_off else 0) for diff, flags, code_off, idx in methods]
                    for diff, flags, where in placed:
                        put_uleb(out, diff)
                        put_uleb(out, flags)
                        put_uleb(out, where)
                    class_data[data_off] = out
                data_off_new = ("class", data_off)
            else:
                data_off_new = 0
            struct.pack_into("<8I", ids, p, class_idx, access, super_idx, interfaces_off, source_idx, annotations_off, 0, static_off)
            class_data.setdefault(("where", i), data_off_new)

        # The class data after all the code.
        code_count = stats["code"]
        where_of = {}
        for key, out in class_data.items():
            if isinstance(key, int):
                where_of[key] = tail_at + len(tail)
                tail.extend(out)
        while len(tail) & 3:
            tail.append(0)
        for i in range(self.classes_n):
            ref = class_data[("where", i)]
            if ref:
                struct.pack_into("<I", ids, ids_at(self.classes_off) + i * 32 + 24, where_of[ref[1]] + delta)

        # A map of the index sections, the rebuilt code (d8 reads code by the map alone) and itself.
        map_at = tail_at + len(tail)
        entries = [(0x0000, 1, 0)]
        for kind, n, off in ((1, self.strings_n, self.strings_off), (2, self.types_n, self.types_off), (3, self.protos_n, self.protos_off),
                             (4, self.fields_n, self.fields_off), (5, self.methods_n, self.methods_off), (6, self.classes_n, self.classes_off)):
            if n:
                entries.append((kind, n, off + shift))
        if code_count:
            entries.append((0x2001, code_count, tail_at + delta))
        entries.append((0x1000, 1, map_at + delta))
        tail += struct.pack("<I", len(entries))
        for kind, n, off in entries:
            tail += struct.pack("<HHII", kind, 0, n, off)

        data = bytes(blob) + bytes(tail)
        body = bytes(ids)
        pad = new_data_off - (0x70 + len(body))
        size = new_data_off + len(data)

        def sect(n, off):
            return (n, off + shift if n else 0)

        header = bytearray(0x70)
        header[0:8] = b"dex\n039\0"
        struct.pack_into("<III", header, 32, size, 0x70, 0x12345678)
        struct.pack_into("<III", header, 44, 0, 0, map_at + delta)
        struct.pack_into("<12I", header, 56, *sect(self.strings_n, self.strings_off), *sect(self.types_n, self.types_off), *sect(self.protos_n, self.protos_off),
                         *sect(self.fields_n, self.fields_off), *sect(self.methods_n, self.methods_off), *sect(self.classes_n, self.classes_off))
        struct.pack_into("<II", header, 104, len(data), new_data_off)
        out = bytes(header) + body + b"\0" * pad + data
        out = out[:12] + hashlib.sha1(out[32:]).digest() + out[32:]
        out = out[:8] + struct.pack("<I", zlib.adler32(out[12:]) & 0xFFFFFFFF) + out[12:]
        return out, stats


def plain_dex_files(data):
    at = 0
    while True:
        at = data.find(b"dex\n03", at)
        if at < 0:
            return
        if data[at + 7:at + 8] == b"\0" and at + 0x70 <= len(data):
            size, header_size, endian = struct.unpack_from("<III", data, at + 32)
            if header_size == 0x70 and endian == 0x12345678 and at + size <= len(data):
                yield data[at:at + size]
                at += size
                continue
        at += 4


def compact_dex_files(data):
    # The .vdex's own header: magic, two versions, how many DEX files, the size of the verifier's notes; a checksum for each
    # file; then the sizes of the DEX files, of the data section they share, and of the quickening section, which is last.
    files, deps_size = struct.unpack_from("<II", data, 12)
    section = 20 + 4 * files
    dex_size, shared_size, quick_size = struct.unpack_from("<III", data, section)
    quick_at = section + 12 + dex_size + shared_size + deps_size if quick_size else 0
    assert quick_at + quick_size <= len(data), "the quickening section runs past the file"
    at = 0
    while True:
        at = data.find(b"cdex001\0", at)
        if at < 0:
            return
        if struct.unpack_from("<I", data, at + 40)[0] == 0x12345678:
            yield Compact(data, at, quick_at)
        at += 8


def main():
    src, out = sys.argv[1], sys.argv[2]
    wanted = set(sys.argv[3:])
    os.makedirs(out, exist_ok=True)
    total = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".vdex"):
            continue
        jar = "core-oj" if name == "boot.vdex" else name[len("boot-"):-len(".vdex")]
        if wanted and jar not in wanted:
            continue
        data = open(os.path.join(src, name), "rb").read()
        if data[:4] != b"vdex":
            print("%s: not a .vdex" % name)
            continue
        files = [(dex, None) for dex in plain_dex_files(data)]
        if not files:
            files = [c.convert() for c in compact_dex_files(data)]
        if not files:
            print("%s (version %s): no DEX inside" % (name, data[4:7].decode("ascii", "replace")))
            continue
        for i, (dex, stats) in enumerate(files):
            target = "%s%s.dex" % (jar, "" if i == 0 else "-classes%d" % (i + 1))
            open(os.path.join(out, target), "wb").write(dex)
            total += len(dex)
            classes = struct.unpack_from("<I", dex, 96)[0]
            how = "as it was" if stats is None else ("from compact DEX: %d methods, %d fields, %d code items (%d of them shared before), %d hidden-API marks "
                                                     "taken off, %d instructions unquickened") % (stats["methods"], stats["fields"], stats["code"], stats["shared"],
                                                                                        stats["marked"], stats["unquickened"])
            print("%-40s %9d bytes, %5d classes, %s" % (target, len(dex), classes, how))
    print("%d bytes of DEX in %s" % (total, out))


main()
