#!/usr/bin/env python3
"""
Which native methods a boot class path declares, and who must answer for them.

    python tools/dvm-natives.py <art/runtime/native dir> <file.dex>... [--all]

A native method of libcore is implemented in one of two places on Android: in
libcore's own libraries (libjavacore.so, libopenjdk.so), which register theirs
when they are loaded, or inside the runtime (art/runtime/native/*.cc). The
second kind is this port's to write (core/src/dvm/hk_dvm_art.c). This lists,
for each class the runtime answers for in Android's source, the native methods
the DEX files declare; with --all, every class that declares any.

The classes are read from the REGISTER_NATIVE_METHODS("...") lines of the .cc
files, the methods from the DEX files. Nothing is run.
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexasm  # noqa: E402


def natives_of(path):
    with open(path, 'rb') as fh:
        d = dexasm.Dex(fh.read())
    out = {}
    for i in range(d.nc):
        cls, flags, sup, ifaces, source, annos, cdata, statics = d.class_def(i)
        sf, inf, dm, vm = d.class_data(cdata)
        for mi, fl, coff in dm + vm:
            if fl & 0x100:
                c, p, n = d.method(mi)
                out.setdefault(d.type(cls)[1:-1], []).append(('static ' if fl & 8 else '') + d.string(n) + d.proto_text(p))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip())
        return 2
    want_all = '--all' in argv
    art = set()
    for cc in glob.glob(os.path.join(argv[1], '*.cc')):
        with open(cc, encoding='utf-8', errors='replace') as fh:
            art.update(re.findall(r'REGISTER_NATIVE_METHODS\("([^"]+)"\)', fh.read()))
    declared = {}
    for path in argv[2:]:
        if path.startswith('--'):
            continue
        for cls, methods in natives_of(path).items():
            declared.setdefault(cls, []).extend(methods)
    total = 0
    for cls in sorted(declared):
        mine = cls in art
        if not mine and not want_all:
            continue
        total += len(declared[cls])
        print('%s %s (%d)' % ('runtime ' if mine else 'library ', cls, len(declared[cls])))
        for m in sorted(declared[cls]):
            print('    ' + m)
    print('%d native methods in %d classes; the runtime answers for %d classes' % (total, sum(1 for c in declared if want_all or c in art), len(art)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
