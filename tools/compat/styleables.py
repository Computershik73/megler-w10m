r"""The arrays of android.R.styleable, out of the platform's own framework.dex.

    python tools/compat/styleables.py build/android9/boot [--check]

android.R (core/framework/src/android/R.java) was generated from the platform's signatures: the index constants of
R.styleable are there (Theme_windowNoTitle = 4...), but its arrays are not, since the platform makes them in
R$styleable's static initializer, and they were left null. An app that reads a theme by the platform's own array
(android.R.styleable is hidden API, but Android's own apps use it: Settings reads android.R.styleable.Theme in
SettingsLib's SettingsDrawerActivity) read the length of null.

This reads framework.dex (and its classes2, classes3), finds android.R$styleable, and walks its <clinit>: each array is
a new-array, a fill-array-data and a sput-object. It writes core/framework/src/megler/Styleables.java, which holds them
as text, and gives each `public static int[] NAME;` of R.java's styleable class its array
(`= megler.Styleables.get("NAME")`), and its index constants the platform's values where they differ (R.java was made
from another release's signatures: 64 of them did, in the manifest's and app widgets' styleables). With --check it only says what it would change, and whether R.java's index
constants are the platform's.
"""
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R_JAVA = os.path.join(ROOT, 'core', 'framework', 'src', 'android', 'R.java')
OUT_JAVA = os.path.join(ROOT, 'core', 'framework', 'src', 'megler', 'Styleables.java')
TARGET = 'Landroid/R$styleable;'


def uleb(b, p):
    v = shift = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7F) << shift
        if c < 0x80:
            return v, p
        shift += 7


class Dex:
    def __init__(self, data):
        self.b = data
        (self.n_str, self.o_str, self.n_type, self.o_type, _np, _op, self.n_field, self.o_field, self.n_meth, self.o_meth,
         self.n_class, self.o_class) = struct.unpack_from('<12I', data, 0x38)

    def string(self, i):
        off = struct.unpack_from('<I', self.b, self.o_str + 4 * i)[0]
        _, p = uleb(self.b, off)
        end = self.b.index(b'\0', p)
        return self.b[p:end].decode('utf-8', 'replace')

    def type(self, i):
        return self.string(struct.unpack_from('<I', self.b, self.o_type + 4 * i)[0])

    def field(self, i):
        cls, typ, name = struct.unpack_from('<HHI', self.b, self.o_field + 8 * i)
        return self.type(cls), self.type(typ), self.string(name)

    def method_name(self, i):
        _cls, _proto, name = struct.unpack_from('<HHI', self.b, self.o_meth + 8 * i)
        return self.string(name)

    def find_class(self, descriptor):
        for k in range(self.n_class):
            vals = struct.unpack_from('<8I', self.b, self.o_class + 32 * k)
            if self.type(vals[0]) == descriptor:
                return vals
        return None


# Lengths in 16-bit units of the instructions a static initializer of constant arrays is made of.
LENGTHS = {0x00: 1, 0x01: 1, 0x0e: 1, 0x12: 1, 0x13: 2, 0x14: 3, 0x15: 2, 0x23: 2, 0x26: 3, 0x4b: 2, 0x67: 2, 0x69: 2, 0x6d: 2}


def arrays_and_constants(dex, cls):
    """{name: [ints]} from <clinit>, {name: int} from the static values."""
    b = dex.b
    class_data, static_values = cls[6], cls[7]
    p = class_data
    n_static, p = uleb(b, p)
    n_inst, p = uleb(b, p)
    n_direct, p = uleb(b, p)
    _n_virtual, p = uleb(b, p)
    statics = []
    idx = 0
    for _ in range(n_static):
        d, p = uleb(b, p)
        _acc, p = uleb(b, p)
        idx += d
        statics.append(idx)
    for _ in range(n_inst):
        _d, p = uleb(b, p)
        _acc, p = uleb(b, p)
    clinit = None
    idx = 0
    for _ in range(n_direct):
        d, p = uleb(b, p)
        _acc, p = uleb(b, p)
        code, p = uleb(b, p)
        idx += d
        if dex.method_name(idx) == '<clinit>':
            clinit = code
    constants = {}
    if static_values:
        q = static_values
        size, q = uleb(b, q)
        for k in range(size):
            head = b[q]
            q += 1
            vtype, varg = head & 0x1F, head >> 5
            if vtype in (0x1e, 0x1f):  # null, a boolean: the value is in the header
                continue
            if vtype in (0x1c, 0x1d):
                raise SystemExit('static value %d: an array or an annotation, which R$styleable does not have' % k)
            raw = b[q:q + varg + 1]
            q += varg + 1
            if vtype == 0x04 and k < len(statics):
                v = int.from_bytes(raw, 'little', signed=False)
                if raw[-1] & 0x80:
                    v -= 1 << (8 * len(raw))
                constants[dex.field(statics[k])[2]] = v
    arrays = {}
    if clinit is None:
        return arrays, constants
    insns_size = struct.unpack_from('<I', b, clinit + 12)[0]
    base = clinit + 16
    units = struct.unpack_from('<%dH' % insns_size, b, base)
    reg_payload = {}
    value = {}  # what a register holds of the constants loaded
    pc = 0
    while pc < insns_size:
        u = units[pc]
        op = u & 0xFF
        if op == 0x00 and u in (0x0100, 0x0200, 0x0300):
            break  # the payloads, after the code
        if op not in LENGTHS:
            raise SystemExit('<clinit>: opcode 0x%02x at %d is not one this reads' % (op, pc))
        if op == 0x12:
            v = u >> 12
            value[u >> 8 & 0x0F] = v - 16 if v >= 8 else v
        elif op == 0x13:
            v = units[pc + 1]
            value[u >> 8] = v - 65536 if v >= 32768 else v
        elif op == 0x14:
            v = units[pc + 1] | (units[pc + 2] << 16)
            value[u >> 8] = v - (1 << 32) if v >= 1 << 31 else v
        elif op == 0x15:
            v = units[pc + 1] << 16
            value[u >> 8] = v - (1 << 32) if v >= 1 << 31 else v
        elif op == 0x4b:
            # aput vAA, vBB[vCC]: an element at a time, for the arrays dx did not fill from a table
            src, arr, at = u >> 8, units[pc + 1] & 0xFF, units[pc + 1] >> 8
            a = reg_payload.get(arr)
            if a is None or at not in value or src not in value or value[at] >= len(a):
                raise SystemExit('aput at %d: an array or a value this did not follow' % pc)
            a[value[at]] = value[src]
        elif op == 0x26:
            reg = u >> 8
            off = units[pc + 1] | (units[pc + 2] << 16)
            if off >= 1 << 31:
                off -= 1 << 32
            at = pc + off
            ident, width = units[at], units[at + 1]
            count = units[at + 2] | (units[at + 3] << 16)
            if ident != 0x0300 or width != 4:
                raise SystemExit('fill-array-data at %d: ident %04x width %d' % (pc, ident, width))
            reg_payload[reg] = list(struct.unpack_from('<%di' % count, b, base + 2 * (at + 4)))
        elif op == 0x23:
            reg, size_reg = u >> 8 & 0x0F, u >> 12
            if size_reg not in value:
                raise SystemExit('new-array at %d: a size this did not follow' % pc)
            reg_payload[reg] = [0] * value[size_reg]  # filled by what follows
        elif op == 0x69:
            reg = u >> 8
            fcls, ftyp, fname = dex.field(units[pc + 1])
            if fcls == TARGET and ftyp == '[I':
                arrays[fname] = reg_payload.get(reg, [])
        pc += LENGTHS[op]
    return arrays, constants


def platform(boot):
    for name in sorted(os.listdir(boot)):
        if not re.match(r'framework(-classes\d+)?\.dex$', name):
            continue
        dex = Dex(open(os.path.join(boot, name), 'rb').read())
        cls = dex.find_class(TARGET)
        if cls:
            return name, arrays_and_constants(dex, cls)
    raise SystemExit('no %s in %s/framework*.dex' % (TARGET, boot))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    boot = args[0] if args else os.path.join(ROOT, 'build', 'android9', 'boot')
    where, (arrays, constants) = platform(boot)
    src = io.open(R_JAVA, encoding='utf-8', newline='').read()
    crlf = '\r\n' in src
    text = src.replace('\r\n', '\n')
    start = text.index('    public static final class styleable {')
    end = text.index('\n    }\n', start)
    body = text[start:end]
    ours = re.findall(r'public static int\[\] (\w+)(?: = megler\.Styleables\.get\("\w+"\))?;', body)
    ints = dict((n, int(v)) for n, v in re.findall(r'public static final int (\w+) = (-?\d+);', body))
    missing = [n for n in ours if n not in arrays]
    differ = [n for n, v in ints.items() if n in constants and constants[n] != v]
    print('%s: %d arrays, %d index constants; R.java: %d arrays (%d not in the platform), %d constants (%d differ)'
          % (where, len(arrays), len(constants), len(ours), len(missing), len(ints), len(differ)))
    for n in differ[:20]:
        print('  differs: %s = %d here, %d in the platform' % (n, ints[n], constants[n]))
    if check:
        return
    # The index constants go with the arrays: where R.java's (of another release) say otherwise, the platform's.
    # Megler's Styleables.java: the arrays as text, in literals short enough for a class file.
    lines = []
    for n in sorted(arrays):
        lines.append('%s=%s;' % (n, ','.join('%x' % (v & 0xFFFFFFFF) for v in arrays[n])))
    chunks, cur = [], ''
    for line in lines:
        if len(cur) + len(line) > 30000:
            chunks.append(cur)
            cur = ''
        cur += line
    if cur:
        chunks.append(cur)
    out = ['// Generated by tools/compat/styleables.py from the platform\'s framework.dex (android.R$styleable\'s static',
           '// initializer); do not edit. The arrays of android.R.styleable, as the platform makes them: R.java\'s styleable',
           '// fields are given theirs from here.',
           'package megler;', '',
           'public final class Styleables {',
           '    private Styleables() {}',
           '    private static java.util.HashMap<String, int[]> sArrays;',
           '    private static final String[] DATA = {']
    for c in chunks:
        out.append('        "%s",' % c)
    out += ['    };', '',
            '    /** The array of that name; an empty one if the platform has none of it. */',
            '    public static synchronized int[] get(String name) {',
            '        if (sArrays == null) {',
            '            sArrays = new java.util.HashMap<>();',
            '            for (String chunk : DATA)',
            '                for (String entry : chunk.split(";")) {',
            '                    int eq = entry.indexOf(\'=\');',
            '                    if (eq <= 0) continue;',
            '                    String list = entry.substring(eq + 1);',
            '                    String[] parts = list.isEmpty() ? new String[0] : list.split(",");',
            '                    int[] a = new int[parts.length];',
            '                    for (int i = 0; i < a.length; i++) a[i] = (int) Long.parseLong(parts[i], 16);',
            '                    sArrays.put(entry.substring(0, eq), a);',
            '                }',
            '        }',
            '        int[] a = sArrays.get(name);',
            '        return a != null ? a : new int[0];',
            '    }',
            '}', '']
    io.open(OUT_JAVA, 'w', encoding='utf-8', newline='').write('\n'.join(out))
    new_body = re.sub(r'public static int\[\] (\w+)(?: = megler\.Styleables\.get\("\w+"\))?;',
                      lambda m: 'public static int[] %s = megler.Styleables.get("%s");' % (m.group(1), m.group(1)), body)
    new_body = re.sub(r'public static final int (\w+) = (-?\d+);',
                      lambda m: 'public static final int %s = %d;' % (m.group(1), constants.get(m.group(1), int(m.group(2)))), new_body)
    text = text[:start] + new_body + text[end:]
    if crlf:
        text = text.replace('\n', '\r\n')
    io.open(R_JAVA, 'w', encoding='utf-8', newline='').write(text)
    print('wrote %s (%d arrays in %d literals) and gave R.java\'s %d arrays theirs' % (os.path.relpath(OUT_JAVA, ROOT), len(arrays), len(chunks), len(ours)))


if __name__ == '__main__':
    main()
