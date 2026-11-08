"""How many instructions of an arm64 library Megler's loader counts as writing x18.

  x18.py LIBRARY [--raw]

Windows on ARM64 keeps the thread's TEB in x18. As the loader maps an ARM64
library it counts the words of its code that write x18 and says them
(core/src/arm64/mg_patch_arm64.c, writes_x18): the code is the executable
sections of the section headers (core/src/ld/mg_ld.c, read_code_sections),
less the spans of data among them that the symbol table's mapping symbols mark
($d where data starts, $x where instructions start again; read_data_spans),
where the library has a symbol table. This is the same count, done on the file,
so that tools/android9-natives.sh can hold every arm64 library it ships to
none: hand-written assembly (BoringSSL's) keeps constants and banners beside its
instructions, and a word of them can read as a write of x18. --raw counts the
code's words without leaving the data out.

Prints the count; exits 1 when it is not 0.
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf import Elf  # noqa: E402

SHT_PROGBITS, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


def writes_x18(w):
    """mg_patch_arm64.c's writes_x18, word for word."""
    rd, rn, rt2, rs = w & 31, (w >> 5) & 31, (w >> 10) & 31, (w >> 16) & 31
    if (w & 0x1C000000) == 0x10000000 or (w & 0x0E000000) == 0x0A000000:
        return rd == 18
    if (w & 0xFFF00000) == 0xD5300000 or (w & 0xFFF80000) == 0xD5280000:
        return rd == 18
    if (w & 0x5F20FC00) == 0x1E200000:
        return rd == 18 and (w >> 16) & 7 not in (2, 3, 7)
    if (w & 0x9FE0EC00) == 0x0E002C00:
        return rd == 18
    if (w & 0x0A000000) != 0x08000000:
        return False
    if (w & 0x3F000000) == 0x08000000:
        load, o1, o2 = (w >> 22) & 1, (w >> 21) & 1, (w >> 23) & 1
        if load:
            return rd == 18 or bool(o1 and not o2 and rt2 == 18)
        return bool((o1 or not o2) and rs == 18)
    if (w & 0x3B000000) == 0x18000000:
        return (w >> 30) != 3 and rd == 18
    if (w & 0x38000000) == 0x28000000:
        if (w >> 23) & 3 in (1, 3) and rn == 18:
            return True
        return bool(not (w >> 26) & 1 and (w >> 22) & 1 and (rd == 18 or rt2 == 18))
    if (w & 0x38000000) == 0x38000000:
        opc, size, op4 = (w >> 22) & 3, w >> 30, (w >> 10) & 3
        unscaled, wide = not (w >> 24) & 1, (w >> 21) & 1
        if unscaled and not wide and op4 in (1, 3) and rn == 18:
            return True
        if (w >> 26) & 1:
            return False
        if unscaled and wide and op4 == 0:
            return rd == 18
        return opc != 0 and not (size == 3 and opc == 2) and rd == 18
    if (w & 0xBF800000) in (0x0C800000, 0x0D800000):
        return rn == 18
    return False


def code_sections(elf):
    """The sections read_code_sections takes: executable PROGBITS in an executable segment's file bytes, up to eight."""
    segs = [p for p in elf.phdrs if p['type'] == 1 and p['flags'] & 1]
    found = []
    for i, s in enumerate(elf.sections):
        if s['type'] != SHT_PROGBITS or s['flags'] & (SHF_ALLOC | SHF_EXECINSTR) != SHF_ALLOC | SHF_EXECINSTR or not s['size']:
            continue
        inside = any(s['addr'] >= p['vaddr'] and s['size'] <= p['filesz'] and s['addr'] - p['vaddr'] <= p['filesz'] - s['size']
                     and s['offset'] >= p['offset'] and s['offset'] - p['offset'] == s['addr'] - p['vaddr'] for p in segs)
        if not inside or len(found) == 8:
            return None
        found.append(i)
    return found or None


def data_spans(elf, code):
    """read_data_spans: [start, end) of each run of data among the code sections, from the mapping symbols."""
    marks = []
    for s in elf.symtab():
        n = s.name
        if s.bind != 'LOCAL' or s.type != 'NOTYPE' or len(n) < 2 or n[0] != '$' or n[1] not in 'dx' or (len(n) > 2 and n[2] != '.'):
            continue
        if s.shndx not in code:
            continue
        sec = elf.sections[s.shndx]
        marks.append((s.value, n[1] == 'd', sec['addr'] + sec['size']))
    marks.sort()
    spans, i = [], 0
    while i < len(marks):
        if not marks[i][1]:
            i += 1
            continue
        start, end, j = marks[i][0], marks[i][2], i + 1
        while j < len(marks) and marks[j][0] < end and marks[j][1]:
            j += 1
        if j < len(marks) and marks[j][0] < end:
            end = marks[j][0]
        if start < end:
            spans.append((start, end))
        i = j
    return spans


def count(path, raw=False):
    elf = Elf(path)
    if elf.e_machine != 183:
        raise SystemExit('x18.py: %s is not an AArch64 library' % path)
    code = code_sections(elf)
    if code:
        ranges = [(elf.sections[i]['addr'], elf.sections[i]['addr'] + elf.sections[i]['size'], elf.sections[i]['offset'] - elf.sections[i]['addr'])
                  for i in code]
        spans = [] if raw else data_spans(elf, set(code))
    else:
        ranges = [(p['vaddr'], p['vaddr'] + p['filesz'], p['offset'] - p['vaddr']) for p in elf.phdrs if p['type'] == 1 and p['flags'] & 1]
        spans = []
    starts = [a for a, _ in spans]
    n = 0
    for lo, hi, delta in ranges:
        a = (lo + 3) & ~3
        while a + 4 <= hi:
            w = int.from_bytes(elf.d[a + delta:a + delta + 4], 'little')
            if writes_x18(w):
                k = bisect.bisect_right(starts, a) - 1
                if k < 0 or a >= spans[k][1]:
                    n += 1
            a += 4
    return n


def main():
    args = [a for a in sys.argv[1:] if a != '--raw']
    if len(args) != 1:
        raise SystemExit(__doc__)
    n = count(args[0], '--raw' in sys.argv[1:])
    print(n)
    return 1 if n else 0


if __name__ == '__main__':
    sys.exit(main())
