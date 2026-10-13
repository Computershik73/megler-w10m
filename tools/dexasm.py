#!/usr/bin/env python3
"""
A small DEX assembler, checker and lister, for the Dalvik runtime's tests (core/src/dvm).

There is no d8, dx or javac on the machine the tests are built on, so their DEX
files are written by hand in smali's syntax and put together here. The subset of
smali understood is what the fixtures use: classes, fields with initial values,
methods with every instruction of the Dalvik instruction set, labels, switch and
array payloads, .catch/.catchall, .line. What is not understood is refused with
the line number, never skipped. The syntax is smali's own on purpose, so that
the same files can be given to the real smali as a cross-check.

    dexasm.py asm -o out.dex a.smali [b.smali ...]
        assemble; the result is checked (as `check` does) before it is written
    dexasm.py check file.dex
        every structural rule of the format this tool knows: the checksum,
        the map, the sort order and uniqueness of each id table, class
        data, and every instruction of every method decoded, bounds-checked and
        encoded again (which must give the same code units)
    dexasm.py dump file.dex [--code] [--class Lsome/Class;]
        classes, fields and methods; with --code, the instructions
    dexasm.py carray -o out.h --name symbol file.dex
        the file as a C array of 32-bit words (so it is aligned), for embedding
    dexasm.py ctable -o out.h
        the instruction table as C, for the interpreter's own decoder

Python 3.8 or newer, no packages.
"""
import hashlib
import os
import struct
import sys
import zlib

NO_INDEX = 0xFFFFFFFF

# ------------------------------------------------------------------ instructions


class Op:
    def __init__(self, code, name, fmt, ref=None):
        self.code, self.name, self.fmt, self.ref = code, name, fmt, ref


FORMAT_UNITS = {
    '10x': 1, '12x': 1, '11n': 1, '11x': 1, '10t': 1, '20t': 2, '22x': 2, '21t': 2, '21s': 2, '21h': 2, '21c': 2, '23x': 2,
    '22b': 2, '22t': 2, '22s': 2, '22c': 2, '30t': 3, '32x': 3, '31i': 3, '31t': 3, '31c': 3, '35c': 3, '3rc': 3, '45cc': 4,
    '4rcc': 4, '51l': 5,
}

_SIMPLE = """
00 nop 10x
01 move 12x
02 move/from16 22x
03 move/16 32x
04 move-wide 12x
05 move-wide/from16 22x
06 move-wide/16 32x
07 move-object 12x
08 move-object/from16 22x
09 move-object/16 32x
0a move-result 11x
0b move-result-wide 11x
0c move-result-object 11x
0d move-exception 11x
0e return-void 10x
0f return 11x
10 return-wide 11x
11 return-object 11x
12 const/4 11n
13 const/16 21s
14 const 31i
15 const/high16 21h
16 const-wide/16 21s
17 const-wide/32 31i
18 const-wide 51l
19 const-wide/high16 21h
1a const-string 21c string
1b const-string/jumbo 31c string
1c const-class 21c type
1d monitor-enter 11x
1e monitor-exit 11x
1f check-cast 21c type
20 instance-of 22c type
21 array-length 12x
22 new-instance 21c type
23 new-array 22c type
24 filled-new-array 35c type
25 filled-new-array/range 3rc type
26 fill-array-data 31t
27 throw 11x
28 goto 10t
29 goto/16 20t
2a goto/32 30t
2b packed-switch 31t
2c sparse-switch 31t
2d cmpl-float 23x
2e cmpg-float 23x
2f cmpl-double 23x
30 cmpg-double 23x
31 cmp-long 23x
32 if-eq 22t
33 if-ne 22t
34 if-lt 22t
35 if-ge 22t
36 if-gt 22t
37 if-le 22t
38 if-eqz 21t
39 if-nez 21t
3a if-ltz 21t
3b if-gez 21t
3c if-gtz 21t
3d if-lez 21t
6e invoke-virtual 35c method
6f invoke-super 35c method
70 invoke-direct 35c method
71 invoke-static 35c method
72 invoke-interface 35c method
74 invoke-virtual/range 3rc method
75 invoke-super/range 3rc method
76 invoke-direct/range 3rc method
77 invoke-static/range 3rc method
78 invoke-interface/range 3rc method
d0 add-int/lit16 22s
d1 rsub-int 22s
d2 mul-int/lit16 22s
d3 div-int/lit16 22s
d4 rem-int/lit16 22s
d5 and-int/lit16 22s
d6 or-int/lit16 22s
d7 xor-int/lit16 22s
d8 add-int/lit8 22b
d9 rsub-int/lit8 22b
da mul-int/lit8 22b
db div-int/lit8 22b
dc rem-int/lit8 22b
dd and-int/lit8 22b
de or-int/lit8 22b
df xor-int/lit8 22b
e0 shl-int/lit8 22b
e1 shr-int/lit8 22b
e2 ushr-int/lit8 22b
fa invoke-polymorphic 45cc method
fb invoke-polymorphic/range 4rcc method
fc invoke-custom 35c callsite
fd invoke-custom/range 3rc callsite
fe const-method-handle 21c handle
ff const-method-type 21c proto
"""


def _build_ops():
    ops = {}

    def add(code, name, fmt, ref=None):
        ops[code] = Op(code, name, fmt, ref)

    for line in _SIMPLE.strip().splitlines():
        p = line.split()
        add(int(p[0], 16), p[1], p[2], p[3] if len(p) > 3 else None)
    kinds = ['', '-wide', '-object', '-boolean', '-byte', '-char', '-short']
    for base, name, fmt, ref in ((0x44, 'aget', '23x', None), (0x4b, 'aput', '23x', None), (0x52, 'iget', '22c', 'field'),
                                 (0x59, 'iput', '22c', 'field'), (0x60, 'sget', '21c', 'field'), (0x67, 'sput', '21c', 'field')):
        for i, k in enumerate(kinds):
            add(base + i, name + k, fmt, ref)
    unary = ['neg-int', 'not-int', 'neg-long', 'not-long', 'neg-float', 'neg-double', 'int-to-long', 'int-to-float', 'int-to-double',
             'long-to-int', 'long-to-float', 'long-to-double', 'float-to-int', 'float-to-long', 'float-to-double', 'double-to-int',
             'double-to-long', 'double-to-float', 'int-to-byte', 'int-to-char', 'int-to-short']
    for i, n in enumerate(unary):
        add(0x7b + i, n, '12x')
    binops = ['add', 'sub', 'mul', 'div', 'rem', 'and', 'or', 'xor', 'shl', 'shr', 'ushr']
    for base, suffix, fmt in ((0x90, '', '23x'), (0xb0, '/2addr', '12x')):
        code = base
        for t in ('int', 'long'):
            for b in binops:
                add(code, '%s-%s%s' % (b, t, suffix), fmt)
                code += 1
        for t in ('float', 'double'):
            for b in binops[:5]:
                add(code, '%s-%s%s' % (b, t, suffix), fmt)
                code += 1
    return ops


OPS = _build_ops()
OPS_BY_NAME = {o.name: o for o in OPS.values()}


class AsmError(Exception):
    pass


def _fits_s(v, bits):
    return -(1 << (bits - 1)) <= v < (1 << (bits - 1))


def _reg(v, bits, what):
    if not 0 <= v < (1 << bits):
        raise AsmError('register v%d does not fit in %d bits (%s)' % (v, bits, what))
    return v


def encode_insn(op, o):
    """The code units of one instruction. `o` holds A, B, C (registers), lit, off, idx, idx2, regs, first, count as the format needs."""
    f, c = op.fmt, op.code
    if f == '10x':
        return [c]
    if f == '12x':
        return [c | _reg(o['A'], 4, f) << 8 | _reg(o['B'], 4, f) << 12]
    if f == '11n':
        if not _fits_s(o['lit'], 4):
            raise AsmError('literal %d does not fit in 4 bits' % o['lit'])
        return [c | _reg(o['A'], 4, f) << 8 | (o['lit'] & 0xF) << 12]
    if f == '11x':
        return [c | _reg(o['A'], 8, f) << 8]
    if f == '10t':
        if not _fits_s(o['off'], 8) or o['off'] == 0:
            raise AsmError('branch offset %d does not fit goto (use goto/16)' % o['off'])
        return [c | (o['off'] & 0xFF) << 8]
    if f == '20t':
        if not _fits_s(o['off'], 16) or o['off'] == 0:
            raise AsmError('branch offset %d does not fit goto/16' % o['off'])
        return [c, o['off'] & 0xFFFF]
    if f == '22x':
        return [c | _reg(o['A'], 8, f) << 8, _reg(o['B'], 16, f)]
    if f == '21t':
        if not _fits_s(o['off'], 16):
            raise AsmError('branch offset %d does not fit in 16 bits' % o['off'])
        return [c | _reg(o['A'], 8, f) << 8, o['off'] & 0xFFFF]
    if f in ('21s', '21h'):
        if not _fits_s(o['lit'], 16):
            raise AsmError('literal %d does not fit in 16 bits' % o['lit'])
        return [c | _reg(o['A'], 8, f) << 8, o['lit'] & 0xFFFF]
    if f == '21c':
        if not 0 <= o['idx'] <= 0xFFFF:
            raise AsmError('index %d does not fit in 16 bits' % o['idx'])
        return [c | _reg(o['A'], 8, f) << 8, o['idx']]
    if f == '23x':
        return [c | _reg(o['A'], 8, f) << 8, _reg(o['B'], 8, f) | _reg(o['C'], 8, f) << 8]
    if f == '22b':
        if not _fits_s(o['lit'], 8):
            raise AsmError('literal %d does not fit in 8 bits' % o['lit'])
        return [c | _reg(o['A'], 8, f) << 8, _reg(o['B'], 8, f) | (o['lit'] & 0xFF) << 8]
    if f == '22t':
        if not _fits_s(o['off'], 16):
            raise AsmError('branch offset %d does not fit in 16 bits' % o['off'])
        return [c | _reg(o['A'], 4, f) << 8 | _reg(o['B'], 4, f) << 12, o['off'] & 0xFFFF]
    if f == '22s':
        if not _fits_s(o['lit'], 16):
            raise AsmError('literal %d does not fit in 16 bits' % o['lit'])
        return [c | _reg(o['A'], 4, f) << 8 | _reg(o['B'], 4, f) << 12, o['lit'] & 0xFFFF]
    if f == '22c':
        if not 0 <= o['idx'] <= 0xFFFF:
            raise AsmError('index %d does not fit in 16 bits' % o['idx'])
        return [c | _reg(o['A'], 4, f) << 8 | _reg(o['B'], 4, f) << 12, o['idx']]
    if f == '30t':
        return [c, o['off'] & 0xFFFF, (o['off'] >> 16) & 0xFFFF]
    if f == '32x':
        return [c, _reg(o['A'], 16, f), _reg(o['B'], 16, f)]
    if f == '31i':
        if not -(1 << 31) <= o['lit'] < (1 << 32):
            raise AsmError('literal %d does not fit in 32 bits' % o['lit'])
        return [c | _reg(o['A'], 8, f) << 8, o['lit'] & 0xFFFF, (o['lit'] >> 16) & 0xFFFF]
    if f == '31t':
        return [c | _reg(o['A'], 8, f) << 8, o['off'] & 0xFFFF, (o['off'] >> 16) & 0xFFFF]
    if f == '31c':
        return [c | _reg(o['A'], 8, f) << 8, o['idx'] & 0xFFFF, (o['idx'] >> 16) & 0xFFFF]
    if f in ('35c', '45cc'):
        regs = list(o['regs'])
        if len(regs) > 5:
            raise AsmError('more than five registers (use the /range form)')
        for r in regs:
            _reg(r, 4, f)
        r = regs + [0] * (5 - len(regs))
        u = [c | r[4] << 8 | len(regs) << 12, o['idx'], r[0] | r[1] << 4 | r[2] << 8 | r[3] << 12]
        if not 0 <= o['idx'] <= 0xFFFF:
            raise AsmError('index %d does not fit in 16 bits' % o['idx'])
        return u + ([o['idx2']] if f == '45cc' else [])
    if f in ('3rc', '4rcc'):
        if not 0 <= o['count'] <= 255:
            raise AsmError('more than 255 registers in a range')
        u = [c | o['count'] << 8, o['idx'], _reg(o['first'], 16, f)]
        return u + ([o['idx2']] if f == '4rcc' else [])
    if f == '51l':
        v = o['lit'] & 0xFFFFFFFFFFFFFFFF
        return [c | _reg(o['A'], 8, f) << 8, v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 48) & 0xFFFF]
    raise AsmError('format %s' % f)


def _s(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode_insn(u, pc):
    """(op, operands, length) for the instruction at u[pc]; op is None for an unused opcode."""
    w = u[pc]
    op = OPS.get(w & 0xFF)
    if op is None:
        return None, {}, 1
    f = op.fmt
    n = FORMAT_UNITS[f]
    if pc + n > len(u):
        raise AsmError('instruction at %d runs past the end of the code' % pc)
    o = {}
    if f == '12x':
        o = {'A': (w >> 8) & 0xF, 'B': w >> 12}
    elif f == '11n':
        o = {'A': (w >> 8) & 0xF, 'lit': _s(w >> 12, 4)}
    elif f == '11x':
        o = {'A': w >> 8}
    elif f == '10t':
        o = {'off': _s(w >> 8, 8)}
    elif f == '20t':
        o = {'off': _s(u[pc + 1], 16)}
    elif f == '22x':
        o = {'A': w >> 8, 'B': u[pc + 1]}
    elif f == '21t':
        o = {'A': w >> 8, 'off': _s(u[pc + 1], 16)}
    elif f in ('21s', '21h'):
        o = {'A': w >> 8, 'lit': _s(u[pc + 1], 16)}
    elif f == '21c':
        o = {'A': w >> 8, 'idx': u[pc + 1]}
    elif f == '23x':
        o = {'A': w >> 8, 'B': u[pc + 1] & 0xFF, 'C': u[pc + 1] >> 8}
    elif f == '22b':
        o = {'A': w >> 8, 'B': u[pc + 1] & 0xFF, 'lit': _s(u[pc + 1] >> 8, 8)}
    elif f == '22t':
        o = {'A': (w >> 8) & 0xF, 'B': w >> 12, 'off': _s(u[pc + 1], 16)}
    elif f == '22s':
        o = {'A': (w >> 8) & 0xF, 'B': w >> 12, 'lit': _s(u[pc + 1], 16)}
    elif f == '22c':
        o = {'A': (w >> 8) & 0xF, 'B': w >> 12, 'idx': u[pc + 1]}
    elif f == '30t':
        o = {'off': _s(u[pc + 1] | u[pc + 2] << 16, 32)}
    elif f == '32x':
        o = {'A': u[pc + 1], 'B': u[pc + 2]}
    elif f == '31i':
        o = {'A': w >> 8, 'lit': _s(u[pc + 1] | u[pc + 2] << 16, 32)}
    elif f == '31t':
        o = {'A': w >> 8, 'off': _s(u[pc + 1] | u[pc + 2] << 16, 32)}
    elif f == '31c':
        o = {'A': w >> 8, 'idx': u[pc + 1] | u[pc + 2] << 16}
    elif f in ('35c', '45cc'):
        count = w >> 12
        r = [u[pc + 2] & 0xF, (u[pc + 2] >> 4) & 0xF, (u[pc + 2] >> 8) & 0xF, u[pc + 2] >> 12, (w >> 8) & 0xF]
        o = {'regs': r[:count] if count <= 5 else r, 'idx': u[pc + 1], 'count': count}
        if f == '45cc':
            o['idx2'] = u[pc + 3]
    elif f in ('3rc', '4rcc'):
        o = {'count': w >> 8, 'idx': u[pc + 1], 'first': u[pc + 2]}
        if f == '4rcc':
            o['idx2'] = u[pc + 3]
    elif f == '51l':
        o = {'A': w >> 8, 'lit': _s(u[pc + 1] | u[pc + 2] << 16 | u[pc + 3] << 32 | u[pc + 4] << 48, 64)}
    return op, o, n


def payload_units(u, pc):
    """The length of a data payload at u[pc] (0 when it is not one)."""
    w = u[pc]
    if w == 0x0100:
        return 4 + u[pc + 1] * 2
    if w == 0x0200:
        return 2 + u[pc + 1] * 4
    if w == 0x0300:
        return 4 + (u[pc + 1] * (u[pc + 2] | u[pc + 3] << 16) + 1) // 2
    return 0


# ----------------------------------------------------------------- small encoders


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def utf16_units(s):
    b = s.encode('utf-16-le', 'surrogatepass')
    return list(struct.unpack('<%dH' % (len(b) // 2), b))


def mutf8(s):
    out = bytearray()
    for c in utf16_units(s):
        if c != 0 and c < 0x80:
            out.append(c)
        elif c < 0x800:
            out += bytes((0xC0 | c >> 6, 0x80 | (c & 0x3F)))
        else:
            out += bytes((0xE0 | c >> 12, 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F)))
    return bytes(out)


def string_key(s):
    """Strings are sorted by their UTF-16 code units."""
    return utf16_units(s)


def split_params(sig):
    """'(I[JLa/B;)V' -> ('V', ('I', '[J', 'La/B;'))"""
    if not sig.startswith('('):
        raise AsmError('not a method signature: %s' % sig)
    i, params = 1, []
    while sig[i] != ')':
        j = i
        while sig[j] == '[':
            j += 1
        if sig[j] == 'L':
            j = sig.index(';', j)
        params.append(sig[i:j + 1])
        i = j + 1
    return sig[i + 1:], tuple(params)


def shorty_of(proto):
    ret, params = proto
    return ''.join((t if t[0] not in 'L[' else 'L') for t in (ret,) + params)


def arg_words(params):
    return sum(2 if p in ('J', 'D') else 1 for p in params)


ACCESS = {
    'public': 0x1, 'private': 0x2, 'protected': 0x4, 'static': 0x8, 'final': 0x10, 'synchronized': 0x20, 'volatile': 0x40,
    'bridge': 0x40, 'transient': 0x80, 'varargs': 0x80, 'native': 0x100, 'interface': 0x200, 'abstract': 0x400, 'strictfp': 0x800,
    'synthetic': 0x1000, 'annotation': 0x2000, 'enum': 0x4000, 'constructor': 0x10000, 'declared-synchronized': 0x20000,
}

# ------------------------------------------------------------------------- smali


class Field:
    def __init__(self, cls, name, ftype, flags, init, where):
        self.cls, self.name, self.type, self.flags, self.init, self.where = cls, name, ftype, flags, init, where


class Method:
    def __init__(self, cls, name, proto, flags, where):
        self.cls, self.name, self.proto, self.flags, self.where = cls, name, proto, flags, where
        self.registers = None
        self.locals = None
        self.items = []            # ('label', name) ('insn', op, operands, where) ('packed', ...) ('sparse', ...) ('array', ...) ('line', n)
        self.catches = []          # (type or None, start label, end label, handler label, where)

    @property
    def ins(self):
        return arg_words(self.proto[1]) + (0 if self.flags & 0x8 else 1)


class Class:
    def __init__(self, name, flags, where):
        self.name, self.flags, self.where = name, flags, where
        self.super = None
        self.ifaces = []
        self.source = None
        self.fields = []
        self.methods = []


def _strip_comment(line):
    out, quote, i = [], None, 0
    while i < len(line):
        ch = line[i]
        if quote:
            out.append(ch)
            if ch == '\\' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 1
            elif ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
            out.append(ch)
        elif ch == '#':
            break
        else:
            out.append(ch)
        i += 1
    return ''.join(out).strip()


def _split_operands(text):
    """Commas split operands, except inside braces, parentheses and quotes."""
    parts, cur, depth, quote, i = [], [], 0, None, 0
    while i < len(text):
        ch = text[i]
        if quote:
            cur.append(ch)
            if ch == '\\' and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 1
            elif ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
            cur.append(ch)
        elif ch in '{(':
            depth += 1
            cur.append(ch)
        elif ch in '})':
            depth -= 1
            cur.append(ch)
        elif ch == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    last = ''.join(cur).strip()
    if last or parts:
        parts.append(last)
    return parts


_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'b': '\b', 'f': '\f', '\\': '\\', '"': '"', "'": "'", '0': '\0'}


def _unescape(body):
    out, i = [], 0
    while i < len(body):
        ch = body[i]
        if ch == '\\':
            i += 1
            e = body[i]
            if e == 'u':
                out.append(chr(int(body[i + 1:i + 5], 16)))
                i += 4
            elif e in _ESCAPES:
                out.append(_ESCAPES[e])
            else:
                raise AsmError('unknown escape \\%s' % e)
        else:
            out.append(ch)
        i += 1
    return ''.join(out)


def parse_string(tok):
    tok = tok.strip()
    if len(tok) < 2 or tok[0] != '"' or tok[-1] != '"':
        raise AsmError('expected a string in double quotes: %s' % tok)
    return _unescape(tok[1:-1])


def _is_float_text(t):
    if t.lower().startswith(('0x', '-0x')):
        return False
    b = t.lstrip('-')
    return b in ('Infinity', 'NaN', 'Infinityf', 'NaNf') or '.' in t or t[-1:] in 'fFdD' or ('e' in t.lower() and t[-1:] not in 'LlsStT')


def parse_literal(tok, wide=False):
    """An integer, as written or as the bits of a float (or of a double when `wide`)."""
    t = tok.strip()
    if t in ('true', 'false'):
        return 1 if t == 'true' else 0
    if t.startswith("'"):
        return ord(_unescape(t[1:-1]))
    if _is_float_text(t):
        single = t[-1] in 'fF'
        v = float(t[:-1] if t[-1] in 'fFdD' else t)
        if single or not wide:
            return struct.unpack('<i', struct.pack('<f', v))[0]
        return struct.unpack('<q', struct.pack('<d', v))[0]
    if t[-1:] in 'LlsStT' and not t.lower().startswith(('0x', '-0x')) or (t.lower().startswith(('0x', '-0x')) and t[-1:] in 'LlsStT'):
        t = t[:-1]
    try:
        return int(t, 0)
    except ValueError:
        raise AsmError('not a number: %s' % tok)


def _parse_reg(tok, m):
    t = tok.strip()
    if len(t) < 2 or t[0] not in 'vp' or not t[1:].isdigit():
        raise AsmError('expected a register: %s' % tok)
    n = int(t[1:])
    if t[0] == 'v':
        return n
    if m.registers is None:
        raise AsmError('.registers or .locals must come before p-registers are used')
    return m.registers - m.ins + n


def _parse_field_ref(tok):
    cls, _, rest = tok.strip().partition('->')
    name, _, ftype = rest.partition(':')
    if not rest or not ftype:
        raise AsmError('expected Lclass;->name:Type, got %s' % tok)
    return (cls, name, ftype)


def _parse_method_ref(tok):
    cls, _, rest = tok.strip().partition('->')
    p = rest.find('(')
    if not rest or p < 0:
        raise AsmError('expected Lclass;->name(Params)Return, got %s' % tok)
    return (cls, rest[:p], split_params(rest[p:]))


def _parse_callsite(tok):
    """name("method", (proto), extra...)@Lclass;->bootstrap(...)...  -- a call site with an invoke-static bootstrap method."""
    head, at, target = tok.strip().partition('@')
    p = head.find('(')
    if not at or p < 0 or not head.endswith(')'):
        raise AsmError('expected name("method", (proto), ...)@Lclass;->bootstrap(...)...')
    args = _split_operands(head[p + 1:-1])
    if len(args) < 2:
        raise AsmError('a call site needs a method name and a prototype')
    extra = []
    for a in args[2:]:
        extra.append(('string', parse_string(a)) if a.startswith('"') else ('int', parse_literal(a)))
    return (head[:p], _parse_method_ref(target), parse_string(args[0]), split_params(args[1]), tuple(extra))


def _flags(words, where):
    v = 0
    for w in words:
        if w not in ACCESS:
            raise AsmError('%s: unknown access flag %s' % (where, w))
        v |= ACCESS[w]
    return v


def parse_smali(path, text):
    """One .smali file -> one Class."""
    cls = None
    m = None
    block = None                   # a payload being read: ['packed', first, [labels]] and so on
    skip_until = None
    lines = text.splitlines()
    for lineno, raw in enumerate(lines, 1):
        where = '%s:%d' % (os.path.basename(path), lineno)
        line = _strip_comment(raw)
        if not line:
            continue
        try:
            if skip_until:
                if line == skip_until:
                    skip_until = None
                continue
            if block is not None:
                if line.startswith('.end '):
                    m.items.append(tuple(block))
                    block = None
                elif block[0] == 'packed':
                    if not line.startswith(':'):
                        raise AsmError('expected a label in .packed-switch')
                    block[2].append(line[1:])
                elif block[0] == 'sparse':
                    key, arrow, label = line.partition('->')
                    if not arrow or not label.strip().startswith(':'):
                        raise AsmError('expected key -> :label in .sparse-switch')
                    block[1].append((parse_literal(key), label.strip()[1:]))
                else:
                    for tok in line.split():
                        block[2].append(parse_literal(tok, wide=block[1] == 8))
                continue
            if line.startswith('.class'):
                words = line.split()[1:]
                if cls is not None:
                    raise AsmError('one class to a file')
                cls = Class(words[-1], _flags(words[:-1], where), where)
            elif cls is None:
                raise AsmError('.class must come first')
            elif line.startswith('.super'):
                cls.super = line.split()[1]
            elif line.startswith('.implements'):
                cls.ifaces.append(line.split()[1])
            elif line.startswith('.source'):
                cls.source = parse_string(line[len('.source'):])
            elif line.startswith('.field'):
                decl, eq, init = line[len('.field'):].partition('=')
                words = decl.split()
                name, _, ftype = words[-1].partition(':')
                if not ftype:
                    raise AsmError('expected name:Type')
                cls.fields.append(Field(cls.name, name, ftype, _flags(words[:-1], where), init.strip() if eq else None, where))
            elif line.startswith('.end field'):
                pass
            elif line.startswith('.method'):
                words = line.split()[1:]
                sig = words[-1]
                p = sig.find('(')
                if p < 0:
                    raise AsmError('expected name(Params)Return')
                flags = _flags(words[:-1], where)
                if sig[0] == '<':
                    flags |= ACCESS['constructor']
                m = Method(cls.name, sig[:p], split_params(sig[p:]), flags, where)
                cls.methods.append(m)
            elif line.startswith('.end method'):
                m = None
            elif line.startswith('.annotation'):
                skip_until = '.end annotation'
            elif m is None:
                raise AsmError('not understood outside a method: %s' % line)
            elif line.startswith('.registers'):
                m.registers = int(line.split()[1], 0)
            elif line.startswith('.locals'):
                m.locals = int(line.split()[1], 0)
                m.registers = m.locals + m.ins
            elif line.startswith('.line'):
                m.items.append(('line', int(line.split()[1], 0)))
            elif line.startswith(('.prologue', '.param', '.end param', '.local', '.end local', '.restart local')):
                pass
            elif line.startswith('.catchall') or line.startswith('.catch'):
                is_all = line.startswith('.catchall')
                rest = line[len('.catchall' if is_all else '.catch'):].strip()
                etype = None
                if not is_all:
                    etype, rest = rest.split(None, 1)
                lb, rb = rest.index('{'), rest.index('}')
                a, dots, b = rest[lb + 1:rb].partition('..')
                handler = rest[rb + 1:].strip()
                if not dots or not handler.startswith(':'):
                    raise AsmError('expected {:start .. :end} :handler')
                m.catches.append((etype, a.strip()[1:], b.strip()[1:], handler[1:], where))
            elif line.startswith('.packed-switch'):
                block = ['packed', parse_literal(line.split()[1]), []]
            elif line.startswith('.sparse-switch'):
                block = ['sparse', []]
            elif line.startswith('.array-data'):
                block = ['array', int(line.split()[1], 0), []]
            elif line.startswith('.'):
                raise AsmError('directive not understood: %s' % line)
            elif line.startswith(':'):
                m.items.append(('label', line[1:]))
            else:
                name, _, rest = line.partition(' ')
                op = OPS_BY_NAME.get(name)
                if op is None:
                    raise AsmError('unknown instruction %s' % name)
                m.items.append(('insn', op, _parse_operands(op, _split_operands(rest.strip()), m), where))
        except AsmError as e:
            raise AsmError('%s: %s' % (where, e))
        except (ValueError, IndexError) as e:
            raise AsmError('%s: cannot parse "%s" (%s)' % (where, line, e))
    if cls is None:
        raise AsmError('%s: no .class' % path)
    return cls


def _parse_operands(op, a, m):
    f = op.fmt
    want = {'10x': 0, '12x': 2, '11n': 2, '11x': 1, '10t': 1, '20t': 1, '30t': 1, '22x': 2, '32x': 2, '21t': 2, '21s': 2, '31i': 2,
            '21h': 2, '51l': 2, '21c': 2, '31c': 2, '23x': 3, '22b': 3, '22s': 3, '22t': 3, '22c': 3, '31t': 2, '35c': 2, '3rc': 2,
            '45cc': 3, '4rcc': 3}[f]
    if len(a) != want:
        raise AsmError('%s takes %d operands, %d given' % (op.name, want, len(a)))
    o = {}

    def label(tok):
        if not tok.startswith(':'):
            raise AsmError('expected a label: %s' % tok)
        return tok[1:]

    def ref(tok):
        k = op.ref
        if k == 'string':
            return ('string', parse_string(tok))
        if k == 'type':
            return ('type', tok.strip())
        if k == 'field':
            return ('field', _parse_field_ref(tok))
        if k == 'method':
            return ('method', _parse_method_ref(tok))
        if k == 'proto':
            return ('proto', split_params(tok.strip()))
        if k == 'callsite':
            return ('callsite', _parse_callsite(tok))
        raise AsmError('%s is not supported by this assembler' % op.name)

    if f in ('12x', '22x', '32x'):
        o = {'A': _parse_reg(a[0], m), 'B': _parse_reg(a[1], m)}
    elif f == '11x':
        o = {'A': _parse_reg(a[0], m)}
    elif f in ('10t', '20t', '30t'):
        o = {'label': label(a[0])}
    elif f in ('21t', '31t'):
        o = {'A': _parse_reg(a[0], m), 'label': label(a[1])}
    elif f in ('11n', '21s', '31i', '51l'):
        o = {'A': _parse_reg(a[0], m), 'lit': parse_literal(a[1], wide=op.name.startswith('const-wide'))}
    elif f == '21h':
        v = parse_literal(a[1], wide=op.name.startswith('const-wide'))
        shift = 48 if op.name.startswith('const-wide') else 16
        if v & ((1 << shift) - 1):
            raise AsmError('%s needs a value whose low %d bits are zero' % (op.name, shift))
        v = (v & ((1 << (shift + 16)) - 1)) >> shift
        o = {'A': _parse_reg(a[0], m), 'lit': _s(v, 16)}
    elif f in ('21c', '31c'):
        o = {'A': _parse_reg(a[0], m), 'ref': ref(a[1])}
    elif f == '23x':
        o = {'A': _parse_reg(a[0], m), 'B': _parse_reg(a[1], m), 'C': _parse_reg(a[2], m)}
    elif f in ('22b', '22s'):
        o = {'A': _parse_reg(a[0], m), 'B': _parse_reg(a[1], m), 'lit': parse_literal(a[2])}
    elif f == '22t':
        o = {'A': _parse_reg(a[0], m), 'B': _parse_reg(a[1], m), 'label': label(a[2])}
    elif f == '22c':
        o = {'A': _parse_reg(a[0], m), 'B': _parse_reg(a[1], m), 'ref': ref(a[2])}
    elif f in ('35c', '45cc'):
        body = a[0].strip()
        if not (body.startswith('{') and body.endswith('}')):
            raise AsmError('expected {registers}')
        o = {'regs': [_parse_reg(t, m) for t in body[1:-1].split(',') if t.strip()], 'ref': ref(a[1])}
    elif f in ('3rc', '4rcc'):
        body = a[0].strip()
        if not (body.startswith('{') and body.endswith('}')):
            raise AsmError('expected {first .. last}')
        inner = body[1:-1].strip()
        if not inner:
            o = {'first': 0, 'count': 0, 'ref': ref(a[1])}
        else:
            lo, dots, hi = inner.partition('..')
            first = _parse_reg(lo, m)
            last = _parse_reg(hi, m) if dots else first
            o = {'first': first, 'count': last - first + 1, 'ref': ref(a[1])}
    if f in ('45cc', '4rcc'):
        o['ref2'] = ('proto', split_params(a[2].strip()))
    return o


# ---------------------------------------------------------------------- building


class Pools:
    def __init__(self):
        self.strings, self.types, self.protos, self.fields, self.methods = set(), set(), set(), set(), set()
        self.callsites = []
        self.handles = []

    def string(self, s):
        self.strings.add(s)

    def type(self, t):
        if not t or t[0] not in 'VZBSCIJFDL[':
            raise AsmError('not a type descriptor: %s' % t)
        self.types.add(t)
        self.strings.add(t)

    def proto(self, p):
        self.protos.add(p)
        self.string(shorty_of(p))
        self.type(p[0])
        for t in p[1]:
            self.type(t)

    def field(self, f):
        self.fields.add(f)
        self.type(f[0])
        self.string(f[1])
        self.type(f[2])

    def method(self, m):
        self.methods.add(m)
        self.type(m[0])
        self.string(m[1])
        self.proto(m[2])

    def ref(self, r):
        kind, v = r
        if kind == 'string':
            self.string(v)
        elif kind == 'type':
            self.type(v)
        elif kind == 'field':
            self.field(v)
        elif kind == 'method':
            self.method(v)
        elif kind == 'proto':
            self.proto(v)
        elif kind == 'callsite':
            name, target, mname, proto, extra = v
            self.method(target)
            self.string(mname)
            self.proto(proto)
            for k, x in extra:
                if k == 'string':
                    self.string(x)
            if target not in self.handles:
                self.handles.append(target)
            if v not in self.callsites:
                self.callsites.append(v)

    def freeze(self):
        self.s = sorted(self.strings, key=string_key)
        self.si = {s: i for i, s in enumerate(self.s)}
        self.t = sorted(self.types, key=lambda t: self.si[t])
        self.ti = {t: i for i, t in enumerate(self.t)}
        self.p = sorted(self.protos, key=lambda p: (self.ti[p[0]], [self.ti[x] for x in p[1]]))
        self.pi = {p: i for i, p in enumerate(self.p)}
        self.f = sorted(self.fields, key=lambda f: (self.ti[f[0]], self.si[f[1]], self.ti[f[2]]))
        self.fi = {f: i for i, f in enumerate(self.f)}
        self.m = sorted(self.methods, key=lambda m: (self.ti[m[0]], self.si[m[1]], self.pi[m[2]]))
        self.mi = {m: i for i, m in enumerate(self.m)}

    def index(self, r):
        kind, v = r
        if kind == 'string':
            return self.si[v]
        if kind == 'type':
            return self.ti[v]
        if kind == 'field':
            return self.fi[v]
        if kind == 'method':
            return self.mi[v]
        if kind == 'proto':
            return self.pi[v]
        if kind == 'callsite':
            return self.callsites.index(v)
        raise AsmError('reference kind %s' % kind)


def encoded_value(vtype, v):
    """One encoded_value: ints in as few bytes as hold them, floats with their low zero bytes dropped."""
    if vtype in (0x00, 0x02, 0x04, 0x06):                       # byte short int long: signed
        n = 1
        while not _fits_s(v, 8 * n):
            n += 1
        return bytes([(n - 1) << 5 | vtype]) + (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')
    if vtype in (0x03, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b):   # char and the indexes: unsigned
        n = 1
        while v >> (8 * n):
            n += 1
        return bytes([(n - 1) << 5 | vtype]) + v.to_bytes(n, 'little')
    if vtype in (0x10, 0x11):                                   # float, double: the high bytes
        size = 4 if vtype == 0x10 else 8
        b = (v & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')
        while len(b) > 1 and b[0] == 0:
            b = b[1:]
        return bytes([(len(b) - 1) << 5 | vtype]) + b
    if vtype == 0x1e:
        return bytes([0x1e])
    if vtype == 0x1f:
        return bytes([(1 if v else 0) << 5 | 0x1f])
    raise AsmError('encoded value type %#x' % vtype)


def _static_value(f, pools):
    t = f.type
    if f.init is None or f.init == 'null':
        if t[0] in 'L[':
            return encoded_value(0x1e, 0)
        return encoded_value({'Z': 0x1f, 'B': 0x00, 'S': 0x02, 'C': 0x03, 'I': 0x04, 'J': 0x06, 'F': 0x10, 'D': 0x11}[t], 0)
    if t == 'Ljava/lang/String;':
        return encoded_value(0x17, pools.si[parse_string(f.init)])
    if t == 'Z':
        return encoded_value(0x1f, parse_literal(f.init))
    if t == 'F':
        return encoded_value(0x10, parse_literal(f.init if _is_float_text(f.init) else f.init + '.0f'))
    if t == 'D':
        return encoded_value(0x11, parse_literal(f.init if _is_float_text(f.init) else f.init + '.0', wide=True))
    if t in 'BSCIJ':
        return encoded_value({'B': 0x00, 'S': 0x02, 'C': 0x03, 'I': 0x04, 'J': 0x06}[t], parse_literal(f.init))
    raise AsmError('%s: an initial value for a field of type %s is not supported' % (f.where, t))


def _assemble_code(m, pools):
    """A method's instructions -> (code units, tries, handler table bytes, outs, line table)."""
    # Addresses first: every instruction's size follows from its name, and a payload starts at an even address.
    addr, labels, placed, payload_user = 0, {}, [], {}
    for it in m.items:
        kind = it[0]
        if kind == 'label':
            if it[1] in labels:
                raise AsmError('%s: label :%s defined twice' % (m.where, it[1]))
            labels[it[1]] = addr
        elif kind == 'insn':
            placed.append((addr, it))
            addr += FORMAT_UNITS[it[1].fmt]
        elif kind == 'line':
            placed.append((addr, it))
        else:
            if addr & 1:
                # the label of this payload was put before the padding: move it
                for name, a in labels.items():
                    if a == addr:
                        labels[name] = addr + 1
                placed.append((addr, ('insn', OPS[0], {}, m.where)))
                addr += 1
            placed.append((addr, it))
            if kind == 'packed':
                addr += 4 + 2 * len(it[2])
            elif kind == 'sparse':
                addr += 2 + 4 * len(it[1])
            else:
                addr += 4 + (it[1] * len(it[2]) + 1) // 2
    total = addr

    def target(name, where):
        if name not in labels:
            raise AsmError('%s: no label :%s' % (where, name))
        return labels[name]

    # Switch targets are relative to the switch instruction, so each payload must know who uses it.
    for a, it in placed:
        if it[0] == 'insn' and it[1].fmt == '31t':
            payload_user.setdefault(target(it[2]['label'], it[3]), a)

    units, outs, lines = [], 0, []
    for a, it in placed:
        kind = it[0]
        if kind == 'line':
            lines.append((a, it[1]))
            continue
        assert len(units) == a
        if kind == 'insn':
            op, o, where = it[1], dict(it[2]), it[3]
            try:
                if 'label' in o:
                    o['off'] = target(o['label'], where) - a
                if 'ref' in o:
                    o['idx'] = pools.index(o['ref'])
                if 'ref2' in o:
                    o['idx2'] = pools.index(o['ref2'])
                if op.fmt in ('35c', '45cc') and op.ref in ('method', 'callsite'):
                    outs = max(outs, len(o['regs']))
                if op.fmt in ('3rc', '4rcc') and op.ref in ('method', 'callsite'):
                    outs = max(outs, o['count'])
                for key in ('A', 'B', 'C'):
                    if key in o and o[key] >= m.registers:
                        raise AsmError('register v%d is beyond .registers %d' % (o[key], m.registers))
                units += encode_insn(op, o)
            except AsmError as e:
                raise AsmError('%s: %s' % (where, e))
        elif kind == 'packed':
            if a not in payload_user:
                raise AsmError('%s: a .packed-switch that no instruction uses' % m.where)
            base = payload_user[a]
            units += [0x0100, len(it[2]), it[1] & 0xFFFF, (it[1] >> 16) & 0xFFFF]
            for name in it[2]:
                off = target(name, m.where) - base
                units += [off & 0xFFFF, (off >> 16) & 0xFFFF]
        elif kind == 'sparse':
            if a not in payload_user:
                raise AsmError('%s: a .sparse-switch that no instruction uses' % m.where)
            base = payload_user[a]
            pairs = sorted(it[1])
            units += [0x0200, len(pairs)]
            for key, _ in pairs:
                units += [key & 0xFFFF, (key >> 16) & 0xFFFF]
            for _, name in pairs:
                off = target(name, m.where) - base
                units += [off & 0xFFFF, (off >> 16) & 0xFFFF]
        else:
            width, values = it[1], it[2]
            data = b''.join((v & ((1 << (8 * width)) - 1)).to_bytes(width, 'little') for v in values)
            if len(data) & 1:
                data += b'\0'
            units += [0x0300, width, len(values) & 0xFFFF, len(values) >> 16]
            units += list(struct.unpack('<%dH' % (len(data) // 2), data))
    assert len(units) == total

    # Tries: the directives may nest and overlap; the format wants ranges that do not, each with its ordered list of handlers.
    tries, handler_blob = [], b''
    if m.catches:
        ranges = []
        for etype, a, b, h, where in m.catches:
            s, e = target(a, where), target(b, where)
            if e <= s:
                raise AsmError('%s: an empty .catch range' % where)
            ranges.append((s, e, etype, target(h, where)))
        points = sorted({p for r in ranges for p in r[:2]})
        segs = []
        for s, e in zip(points, points[1:]):
            typed, catch_all = [], None
            for rs, re_, etype, h in ranges:
                if not (rs <= s and e <= re_) or catch_all is not None:
                    continue
                if etype is None:
                    catch_all = h
                elif etype not in [t for t, _ in typed]:
                    typed.append((etype, h))
            if not typed and catch_all is None:
                continue
            key = (tuple(typed), catch_all)
            if segs and segs[-1][1] == s and segs[-1][2] == key:
                segs[-1] = (segs[-1][0], e, key)
            else:
                segs.append((s, e, key))
        keys = []
        for _, _, key in segs:
            if key not in keys:
                keys.append(key)
        blob = bytearray(uleb(len(keys)))
        offs = {}
        for key in keys:
            offs[key] = len(blob)
            typed, catch_all = key
            blob += sleb(-len(typed) if catch_all is not None else len(typed))
            for etype, h in typed:
                blob += uleb(pools.ti[etype]) + uleb(h)
            if catch_all is not None:
                blob += uleb(catch_all)
        handler_blob = bytes(blob)
        tries = [(s, e - s, offs[key]) for s, e, key in segs]
    return units, tries, handler_blob, outs, lines


def _debug_info(m, lines):
    """A debug_info_item with the line table only."""
    out = bytearray(uleb(lines[0][1]) + uleb(len(m.proto[1])))
    for _ in m.proto[1]:
        out += uleb(0)                                           # uleb128p1 of NO_INDEX: no parameter names
    addr, line = 0, lines[0][1]
    for a, n in lines:
        da, dl = a - addr, n - line
        if dl < -4 or dl > 10:
            out += b'\x02' + sleb(dl)
            dl = 0
        if da > 15:
            out += b'\x01' + uleb(da)
            da = 0
        out.append(0x0a + (dl + 4) + 15 * da)
        addr, line = a, n
    out.append(0)
    return bytes(out)


def build_dex(classes, version='035'):
    pools = Pools()
    names = set()
    for c in classes:
        if c.name in names:
            raise AsmError('%s: class %s is defined twice' % (c.where, c.name))
        names.add(c.name)
        pools.type(c.name)
        if c.super:
            pools.type(c.super)
        for i in c.ifaces:
            pools.type(i)
        if c.source:
            pools.string(c.source)
        for f in c.fields:
            pools.field((f.cls, f.name, f.type))
            if f.init and f.type == 'Ljava/lang/String;' and f.init != 'null':
                pools.string(parse_string(f.init))
        for m in c.methods:
            pools.method((m.cls, m.name, m.proto))
            has_code = any(it[0] != 'line' for it in m.items)
            if has_code and m.registers is None:
                raise AsmError('%s: a method with code needs .registers or .locals' % m.where)
            if has_code and (m.flags & 0x500):
                raise AsmError('%s: an abstract or native method has code' % m.where)
            if (m.flags & 0x20) and not (m.flags & 0x100):
                raise AsmError('%s: "synchronized" is a flag for native methods only; a method with code is declared-synchronized and has '
                               'the monitor instructions in it' % m.where)
            if not has_code and not (m.flags & 0x500):
                raise AsmError('%s: a method without code must be abstract or native' % m.where)
            for it in m.items:
                if it[0] == 'insn':
                    for key in ('ref', 'ref2'):
                        if key in it[2]:
                            pools.ref(it[2][key])
            for etype, _, _, _, _ in m.catches:
                if etype:
                    pools.type(etype)
    pools.freeze()
    if pools.callsites and version < '038':
        version = '038'

    # A class comes after its superclass and its interfaces when they are in the same file.
    by_name = {c.name: c for c in classes}
    ordered, seen = [], set()

    def visit(c):
        if c.name in seen:
            return
        seen.add(c.name)
        for dep in ([c.super] if c.super else []) + c.ifaces:
            if dep in by_name:
                visit(by_name[dep])
        ordered.append(c)

    for c in sorted(classes, key=lambda c: pools.ti[c.name]):
        visit(c)

    ns, nt, npr, nf, nm, nc = len(pools.s), len(pools.t), len(pools.p), len(pools.f), len(pools.m), len(ordered)
    off = 0x70
    string_ids_off = off
    off += 4 * ns
    type_ids_off = off
    off += 4 * nt
    proto_ids_off = off
    off += 12 * npr
    field_ids_off = off
    off += 8 * nf
    method_ids_off = off
    off += 8 * nm
    class_defs_off = off
    off += 32 * nc
    call_site_ids_off = off
    off += 4 * len(pools.callsites)
    method_handles_off = off
    off += 8 * len(pools.handles)
    data_off = off
    data = bytearray()
    sections = []                  # (type code, count, absolute offset) of what the data section holds

    def here():
        return data_off + len(data)

    def align4():
        while len(data) & 3:
            data.append(0)

    # type lists
    lists = set()
    for p in pools.p:
        if p[1]:
            lists.add(tuple(pools.ti[t] for t in p[1]))
    for c in ordered:
        if c.ifaces:
            lists.add(tuple(pools.ti[t] for t in c.ifaces))
    list_off = {}
    start = None
    for tl in sorted(lists):
        align4()
        if start is None:
            start = here()
        list_off[tl] = here()
        data += struct.pack('<I', len(tl)) + b''.join(struct.pack('<H', t) for t in tl)
    if lists:
        sections.append((0x1001, len(lists), start))

    # code: assembled first, since a method's debug info is laid down before its code item and both before the class data
    code = {}
    for c in ordered:
        for m in c.methods:
            if any(it[0] != 'line' for it in m.items):
                code[id(m)] = _assemble_code(m, pools)
    debug_off = {}
    start, count = None, 0
    for c in ordered:
        for m in c.methods:
            if id(m) in code and code[id(m)][4]:
                if start is None:
                    start = here()
                debug_off[id(m)] = here()
                data += _debug_info(m, code[id(m)][4])
                count += 1
    if count:
        sections.append((0x2003, count, start))
    code_off = {}
    start, count = None, 0
    for c in ordered:
        for m in c.methods:
            if id(m) not in code:
                continue
            units, tries, handlers, outs, _ = code[id(m)]
            align4()
            if start is None:
                start = here()
            code_off[id(m)] = here()
            data += struct.pack('<HHHHII', m.registers, m.ins, outs, len(tries), debug_off.get(id(m), 0), len(units))
            data += struct.pack('<%dH' % len(units), *units)
            if tries:
                if len(units) & 1:
                    data += b'\0\0'
                for s, n, h in tries:
                    data += struct.pack('<IHH', s, n, h)
                data += handlers
            if m.registers < m.ins:
                raise AsmError('%s: .registers %d is less than the %d the parameters take' % (m.where, m.registers, m.ins))
            count += 1
    if count:
        sections.append((0x2001, count, start))

    # string data
    string_off = []
    start = here()
    for s in pools.s:
        string_off.append(here())
        data += uleb(len(utf16_units(s))) + mutf8(s) + b'\0'
    sections.append((0x2002, ns, start))

    # encoded arrays: static values, then call sites
    static_off = {}
    arrays = []
    for c in ordered:
        statics = sorted((f for f in c.fields if f.flags & 0x8), key=lambda f: pools.fi[(f.cls, f.name, f.type)])
        last = -1
        for i, f in enumerate(statics):
            if f.init is not None:
                last = i
        if last >= 0:
            arrays.append((c.name, uleb(last + 1) + b''.join(_static_value(f, pools) for f in statics[:last + 1])))
    callsite_off = []
    for cs in pools.callsites:
        name, target, mname, proto, extra = cs
        body = encoded_value(0x16, pools.handles.index(target)) + encoded_value(0x17, pools.si[mname]) + encoded_value(0x15, pools.pi[proto])
        for k, x in extra:
            body += encoded_value(0x17, pools.si[x]) if k == 'string' else encoded_value(0x04, x)
        arrays.append((cs, uleb(3 + len(extra)) + body))
    start = here()
    for key, blob in arrays:
        if isinstance(key, str):
            static_off[key] = here()
        else:
            callsite_off.append(here())
        data += blob
    if arrays:
        sections.append((0x2005, len(arrays), start))

    # class data
    class_data_off = {}
    start, count = None, 0
    for c in ordered:
        if not c.fields and not c.methods:
            continue

        def fkey(f):
            return pools.fi[(f.cls, f.name, f.type)]

        def mkey(m):
            return pools.mi[(m.cls, m.name, m.proto)]

        sf = sorted((f for f in c.fields if f.flags & 0x8), key=fkey)
        inf = sorted((f for f in c.fields if not f.flags & 0x8), key=fkey)
        dm = sorted((m for m in c.methods if m.flags & 0x1000A or m.name[0] == '<'), key=mkey)
        vm = sorted((m for m in c.methods if not (m.flags & 0x1000A or m.name[0] == '<')), key=mkey)
        if start is None:
            start = here()
        class_data_off[c.name] = here()
        data += uleb(len(sf)) + uleb(len(inf)) + uleb(len(dm)) + uleb(len(vm))
        for group in (sf, inf):
            prev = 0
            for f in group:
                if fkey(f) == prev and f is not group[0]:
                    raise AsmError('%s: field %s is declared twice' % (f.where, f.name))
                data += uleb(fkey(f) - prev) + uleb(f.flags)
                prev = fkey(f)
        for group in (dm, vm):
            prev = 0
            for m in group:
                if mkey(m) == prev and m is not group[0]:
                    raise AsmError('%s: method %s is declared twice' % (m.where, m.name))
                data += uleb(mkey(m) - prev) + uleb(m.flags) + uleb(code_off.get(id(m), 0))
                prev = mkey(m)
        count += 1
    if count:
        sections.append((0x2000, count, start))

    # the map
    align4()
    map_off = here()
    entries = [(0x0000, 1, 0), (0x0001, ns, string_ids_off), (0x0002, nt, type_ids_off), (0x0003, npr, proto_ids_off),
               (0x0004, nf, field_ids_off), (0x0005, nm, method_ids_off), (0x0006, nc, class_defs_off),
               (0x0007, len(pools.callsites), call_site_ids_off), (0x0008, len(pools.handles), method_handles_off)]
    entries = [e for e in entries if e[1]] + sections + [(0x1000, 1, map_off)]
    entries.sort(key=lambda e: e[2])
    data += struct.pack('<I', len(entries))
    for code_, n, o in entries:
        data += struct.pack('<HHII', code_, 0, n, o)
    align4()

    # the id tables
    ids = bytearray()
    for o in string_off:
        ids += struct.pack('<I', o)
    for t in pools.t:
        ids += struct.pack('<I', pools.si[t])
    for p in pools.p:
        tl = tuple(pools.ti[t] for t in p[1])
        ids += struct.pack('<III', pools.si[shorty_of(p)], pools.ti[p[0]], list_off[tl] if tl else 0)
    for f in pools.f:
        ids += struct.pack('<HHI', pools.ti[f[0]], pools.ti[f[2]], pools.si[f[1]])
    for m in pools.m:
        ids += struct.pack('<HHI', pools.ti[m[0]], pools.pi[m[2]], pools.si[m[1]])
    for c in ordered:
        tl = tuple(pools.ti[t] for t in c.ifaces)
        ids += struct.pack('<IIIIIIII', pools.ti[c.name], c.flags, pools.ti[c.super] if c.super else NO_INDEX, list_off[tl] if tl else 0,
                           pools.si[c.source] if c.source else NO_INDEX, 0, class_data_off.get(c.name, 0), static_off.get(c.name, 0))
    for o in callsite_off:
        ids += struct.pack('<I', o)
    for h in pools.handles:
        ids += struct.pack('<HHHH', 0x04, 0, pools.mi[h], 0)       # invoke-static
    assert 0x70 + len(ids) == data_off

    size = data_off + len(data)
    header = bytearray(0x70)
    header[0:8] = b'dex\n' + version.encode() + b'\0'
    struct.pack_into('<IIIIII', header, 32, size, 0x70, 0x12345678, 0, 0, map_off)
    struct.pack_into('<IIIIIIIIIIII', header, 56, ns, string_ids_off, nt, type_ids_off, npr, proto_ids_off if npr else 0, nf,
                     field_ids_off if nf else 0, nm, method_ids_off if nm else 0, nc, class_defs_off if nc else 0)
    struct.pack_into('<II', header, 104, len(data), data_off)
    out = bytearray(header + ids + data)
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into('<I', out, 8, zlib.adler32(bytes(out[12:])) & 0xFFFFFFFF)
    return bytes(out)


# ----------------------------------------------------------------------- reading


class Dex:
    """A DEX file read back, with nothing taken on trust: every offset is bounds-checked."""

    def __init__(self, b):
        self.b = b
        if len(b) < 0x70 or b[:4] != b'dex\n':
            raise AsmError('not a DEX file')
        self.version = b[4:7].decode('ascii', 'replace')
        (self.checksum,) = struct.unpack_from('<I', b, 8)
        self.signature = b[12:32]
        (self.file_size, self.header_size, self.endian, self.link_size, self.link_off, self.map_off) = struct.unpack_from('<IIIIII', b, 32)
        (self.ns, self.s_off, self.nt, self.t_off, self.np, self.p_off, self.nf, self.f_off, self.nm, self.m_off, self.nc, self.c_off,
         self.data_size, self.data_off) = struct.unpack_from('<IIIIIIIIIIIIII', b, 56)
        self._strings = {}

    def u16(self, o):
        return struct.unpack_from('<H', self.b, o)[0]

    def u32(self, o):
        return struct.unpack_from('<I', self.b, o)[0]

    def uleb(self, o):
        r, s = 0, 0
        while True:
            c = self.b[o]
            o += 1
            r |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return r, o

    def sleb(self, o):
        r, s = 0, 0
        while True:
            c = self.b[o]
            o += 1
            r |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                if c & 0x40:
                    r -= 1 << s
                return r, o

    def string_units(self, i):
        """A string as its UTF-16 code units."""
        if i in self._strings:
            return self._strings[i]
        if i >= self.ns:
            raise AsmError('string index %d of %d' % (i, self.ns))
        n, o = self.uleb(self.u32(self.s_off + 4 * i))
        units = []
        while True:
            c = self.b[o]
            o += 1
            if c == 0:
                break
            if c < 0x80:
                units.append(c)
            elif c & 0xE0 == 0xC0:
                units.append((c & 0x1F) << 6 | (self.b[o] & 0x3F))
                o += 1
            else:
                units.append((c & 0x0F) << 12 | (self.b[o] & 0x3F) << 6 | (self.b[o + 1] & 0x3F))
                o += 2
        if len(units) != n:
            raise AsmError('string %d says %d UTF-16 units and has %d' % (i, n, len(units)))
        self._strings[i] = units
        return units

    def string(self, i):
        return struct.pack('<%dH' % len(self.string_units(i)), *self.string_units(i)).decode('utf-16-le', 'surrogatepass')

    def type(self, i):
        if i >= self.nt:
            raise AsmError('type index %d of %d' % (i, self.nt))
        return self.string(self.u32(self.t_off + 4 * i))

    def type_list(self, off):
        if not off:
            return []
        n = self.u32(off)
        return [self.u16(off + 4 + 2 * k) for k in range(n)]

    def proto(self, i):
        if i >= self.np:
            raise AsmError('proto index %d of %d' % (i, self.np))
        shorty, ret, params = struct.unpack_from('<III', self.b, self.p_off + 12 * i)
        return shorty, ret, self.type_list(params)

    def proto_text(self, i):
        _, ret, params = self.proto(i)
        return '(%s)%s' % (''.join(self.type(t) for t in params), self.type(ret))

    def field(self, i):
        if i >= self.nf:
            raise AsmError('field index %d of %d' % (i, self.nf))
        return struct.unpack_from('<HHI', self.b, self.f_off + 8 * i)

    def field_text(self, i):
        c, t, n = self.field(i)
        return '%s->%s:%s' % (self.type(c), self.string(n), self.type(t))

    def method(self, i):
        if i >= self.nm:
            raise AsmError('method index %d of %d' % (i, self.nm))
        return struct.unpack_from('<HHI', self.b, self.m_off + 8 * i)

    def method_text(self, i):
        c, p, n = self.method(i)
        return '%s->%s%s' % (self.type(c), self.string(n), self.proto_text(p))

    def class_def(self, i):
        return struct.unpack_from('<IIIIIIII', self.b, self.c_off + 32 * i)

    def class_data(self, off):
        """([static fields], [instance fields], [direct methods], [virtual methods]) as (index, flags[, code offset])."""
        if not off:
            return [], [], [], []
        o = off
        counts = []
        for _ in range(4):
            v, o = self.uleb(o)
            counts.append(v)
        groups = []
        for g, n in enumerate(counts):
            idx, items = 0, []
            for k in range(n):
                d, o = self.uleb(o)
                idx += d
                flags, o = self.uleb(o)
                if g >= 2:
                    code, o = self.uleb(o)
                    items.append((idx, flags, code))
                else:
                    items.append((idx, flags))
            groups.append(items)
        return tuple(groups)

    def code(self, off):
        regs, ins, outs, ntries, debug, n = struct.unpack_from('<HHHHII', self.b, off)
        if off + 16 + 2 * n > len(self.b):
            raise AsmError('code item at %#x runs past the end of the file' % off)
        units = list(struct.unpack_from('<%dH' % n, self.b, off + 16))
        tries, handlers = [], {}
        if ntries:
            t = off + 16 + 2 * n + (2 if n & 1 else 0)
            hbase = t + 8 * ntries
            for k in range(ntries):
                start, count, hoff = struct.unpack_from('<IHH', self.b, t + 8 * k)
                o = hbase + hoff
                size, o = self.sleb(o)
                typed = []
                for _ in range(abs(size)):
                    ti, o = self.uleb(o)
                    addr, o = self.uleb(o)
                    typed.append((ti, addr))
                catch_all = None
                if size <= 0:
                    catch_all, o = self.uleb(o)
                tries.append((start, count, typed, catch_all))
        return regs, ins, outs, debug, units, tries


def walk_code(units):
    """Every instruction and payload of a method, in address order: (pc, op or None, operands, length, is_payload)."""
    out, pc = [], 0
    while pc < len(units):
        n = payload_units(units, pc) if units[pc] & 0xFF == 0 and units[pc] != 0 else 0
        if n:
            if pc + n > len(units):
                raise AsmError('payload at %d runs past the end of the code' % pc)
            out.append((pc, None, {}, n, True))
        else:
            op, o, n = decode_insn(units, pc)
            out.append((pc, op, o, n, False))
        pc += n
    return out


def check_dex(b, label):
    """Raises AsmError at the first rule broken; returns a line of counts."""
    d = Dex(b)

    def bad(msg):
        raise AsmError('%s: %s' % (label, msg))

    if d.version not in ('035', '037', '038', '039'):
        bad('version %s' % d.version)
    if d.file_size != len(b):
        bad('file_size %d, file has %d bytes' % (d.file_size, len(b)))
    if d.header_size != 0x70 or d.endian != 0x12345678:
        bad('header size or endian tag')
    if zlib.adler32(b[12:]) & 0xFFFFFFFF != d.checksum:
        bad('checksum')
    # The signature is the SHA-1 of what follows it. Android does not check it and d8 does not always write it so: noted, not refused.
    signed = hashlib.sha1(b[32:]).digest() == d.signature
    if d.data_off + d.data_size != len(b) or d.data_size & 3:
        bad('data section %#x+%#x in a file of %#x bytes' % (d.data_off, d.data_size, len(b)))

    # the map: sorted by offset, agreeing with the header
    n = d.u32(d.map_off)
    entries = [struct.unpack_from('<HHII', b, d.map_off + 4 + 12 * k) for k in range(n)]
    if [e[3] for e in entries] != sorted(e[3] for e in entries) or len({e[0] for e in entries}) != n:
        bad('map entries out of order or repeated')
    in_map = {e[0]: (e[2], e[3]) for e in entries}
    for code_, count, off in ((1, d.ns, d.s_off), (2, d.nt, d.t_off), (3, d.np, d.p_off), (4, d.nf, d.f_off), (5, d.nm, d.m_off), (6, d.nc, d.c_off)):
        if count and in_map.get(code_) != (count, off):
            bad('map entry for section %d is %s, header says %s' % (code_, in_map.get(code_), (count, off)))
        if not count and code_ in in_map:
            bad('map has an empty section %d' % code_)
    if in_map.get(0) != (1, 0) or in_map.get(0x1000) != (1, d.map_off):
        bad('map lacks the header or itself')

    # id tables: sorted, nothing twice
    strings = [d.string_units(i) for i in range(d.ns)]
    for i in range(1, d.ns):
        if not strings[i - 1] < strings[i]:
            bad('string ids %d and %d are not in order' % (i - 1, i))
    for i in range(d.ns):
        if not d.data_off <= d.u32(d.s_off + 4 * i) < len(b):
            bad('string %d lies outside the data section' % i)
    tids = [d.u32(d.t_off + 4 * i) for i in range(d.nt)]
    if any(a >= c for a, c in zip(tids, tids[1:])) or any(t >= d.ns for t in tids):
        bad('type ids not in order')
    protos = [(d.proto(i)[1], d.proto(i)[2]) for i in range(d.np)]
    if any(a >= c for a, c in zip(protos, protos[1:])):
        bad('proto ids not in order')
    for i in range(d.np):
        shorty, ret, params = d.proto(i)
        want = ''.join((d.type(t) if d.type(t)[0] not in 'L[' else 'L') for t in [ret] + params)
        if d.string(shorty) != want:
            bad('proto %d has shorty %s, its types say %s' % (i, d.string(shorty), want))
    fields = [(c, nm, t) for c, t, nm in (d.field(i) for i in range(d.nf))]
    if any(a >= c for a, c in zip(fields, fields[1:])):
        bad('field ids not in order')
    methods = [(c, nm, p) for c, p, nm in (d.method(i) for i in range(d.nm))]
    if any(a >= c for a, c in zip(methods, methods[1:])):
        bad('method ids not in order')
    for c, nm, p in methods:
        if c >= d.nt or nm >= d.ns or p >= d.np:
            bad('a method id points outside its tables')

    # classes
    defined = {}
    ncode = ninsn = nfields = nmethods = 0
    for i in range(d.nc):
        cls, flags, sup, ifaces, source, annos, cdata, statics = d.class_def(i)
        name = d.type(cls)
        if cls in defined:
            bad('class %s is defined twice' % name)
        later = {d.class_def(k)[0] for k in range(i + 1, d.nc)}
        for dep in ([sup] if sup != NO_INDEX else []) + d.type_list(ifaces):
            if dep in later:
                bad('class %s comes before %s, which it extends or implements' % (name, d.type(dep)))
        defined[cls] = i
        sf, inf, dm, vm = d.class_data(cdata)
        for group, what in ((sf, 'static field'), (inf, 'instance field')):
            idx = [x[0] for x in group]
            if idx != sorted(set(idx)):
                bad('%s: %ss not in order' % (name, what))
            for fi, fl in group:
                if d.field(fi)[0] != cls:
                    bad('%s: a %s of another class' % (name, what))
                if bool(fl & 0x8) != (what == 'static field'):
                    bad('%s: field %s is in the wrong list' % (name, d.field_text(fi)))
        nfields += len(sf) + len(inf)
        for group, what in ((dm, 'direct'), (vm, 'virtual')):
            idx = [x[0] for x in group]
            if idx != sorted(set(idx)):
                bad('%s: %s methods not in order' % (name, what))
            for mi, fl, coff in group:
                mc, mp, mn = d.method(mi)
                mname = d.string(mn)
                if mc != cls:
                    bad('%s: a method of another class' % name)
                is_direct = bool(fl & 0xA) or mname[0] == '<'
                if is_direct != (what == 'direct'):
                    bad('%s: method %s is in the wrong list' % (name, mname))
                if bool(coff) == bool(fl & 0x500):
                    bad('%s.%s: code and the abstract/native flags disagree' % (name, mname))
                if (fl & 0x20) and not (fl & 0x100):
                    bad('%s.%s: the synchronized flag on a method that is not native' % (name, mname))
                nmethods += 1
                if not coff:
                    continue
                ncode += 1
                regs, ins, outs, debug, units, tries = d.code(coff)
                _, ret, params = d.proto(mp)
                want_ins = arg_words([d.type(t) for t in params]) + (0 if fl & 0x8 else 1)
                where = '%s.%s%s' % (name, mname, d.proto_text(mp))
                if ins != want_ins or regs < ins:
                    bad('%s: ins %d (the signature needs %d), registers %d' % (where, ins, want_ins, regs))
                insns = walk_code(units)
                starts = {pc for pc, _, _, _, payload in insns if not payload}
                payloads = {pc for pc, _, _, _, payload in insns if payload}
                for pc, op, o, n, payload in insns:
                    if payload:
                        if pc & 1:
                            bad('%s: payload at odd address %d' % (where, pc))
                        continue
                    if op is None:
                        bad('%s: unused opcode %#x at %d' % (where, units[pc] & 0xFF, pc))
                    ninsn += 1
                    back = encode_insn(op, o) if op.fmt not in ('35c', '45cc') or o['count'] <= 5 else None
                    if back != units[pc:pc + n]:
                        bad('%s: %s at %d encodes back as %s, the file has %s' % (where, op.name, pc, back, units[pc:pc + n]))
                    wide = ('-wide' in op.name or 'long' in op.name or 'double' in op.name) and op.fmt in ('12x', '22x', '32x', '11x', '23x', '21s', '31i', '51l', '21h', '21c', '22c')
                    for key in ('A', 'B', 'C'):
                        if key in o and o[key] + (1 if wide and key == 'A' and op.name not in ('cmp-long', 'cmpl-double', 'cmpg-double', 'long-to-int', 'double-to-int', 'long-to-float', 'double-to-float') else 0) >= regs:
                            bad('%s: %s at %d uses v%d of %d registers' % (where, op.name, pc, o[key], regs))
                    for r in o.get('regs', []):
                        if r >= regs:
                            bad('%s: %s at %d uses v%d of %d registers' % (where, op.name, pc, r, regs))
                    if 'first' in o and o['count'] and o['first'] + o['count'] > regs:
                        bad('%s: %s at %d uses a range beyond %d registers' % (where, op.name, pc, regs))
                    if 'off' in o:
                        t = pc + o['off']
                        if op.fmt == '31t':
                            if t not in payloads:
                                bad('%s: %s at %d does not point at a payload' % (where, op.name, pc))
                            kind = units[t]
                            if kind in (0x0100, 0x0200):
                                size = units[t + 1]
                                tgts = units[t + 4:t + 4 + 2 * size] if kind == 0x0100 else units[t + 2 + 2 * size:t + 2 + 4 * size]
                                for k in range(size):
                                    dest = pc + _s(tgts[2 * k] | tgts[2 * k + 1] << 16, 32)
                                    if dest not in starts:
                                        bad('%s: switch at %d has a target %d that is not an instruction' % (where, pc, dest))
                                if kind == 0x0200:
                                    keys = [_s(units[t + 2 + 2 * k] | units[t + 3 + 2 * k] << 16, 32) for k in range(size)]
                                    if keys != sorted(set(keys)):
                                        bad('%s: sparse switch keys not in order' % where)
                        elif t not in starts:
                            bad('%s: %s at %d branches to %d, which is not an instruction' % (where, op.name, pc, t))
                    if 'idx' in o:
                        limit = {'string': d.ns, 'type': d.nt, 'field': d.nf, 'method': d.nm, 'proto': d.np}.get(op.ref)
                        if limit is not None and o['idx'] >= limit:
                            bad('%s: %s at %d has index %d of %d' % (where, op.name, pc, o['idx'], limit))
                prev_end = 0
                for start, count, typed, catch_all in tries:
                    if start < prev_end or start + count > len(units):
                        bad('%s: try ranges overlap or run past the code' % where)
                    prev_end = start + count
                    for ti, addr in typed:
                        if ti >= d.nt or addr not in starts:
                            bad('%s: a catch handler at %d is not an instruction' % (where, addr))
                    if catch_all is not None and catch_all not in starts:
                        bad('%s: a catch-all handler at %d is not an instruction' % (where, catch_all))
    return '%s: version %s, %d strings, %d types, %d protos, %d fields, %d methods, %d classes; %d fields and %d methods defined, %d with code, %d instructions%s' % (
        label, d.version, d.ns, d.nt, d.np, d.nf, d.nm, d.nc, nfields, nmethods, ncode, ninsn, '' if signed else ' (its signature is not its SHA-1)')


# ----------------------------------------------------------------------- listing


def _flag_words(flags, method):
    out = []
    for name, bit in ACCESS.items():
        if name in ('bridge', 'varargs') and not method:
            continue
        if name in ('volatile', 'transient') and method:
            continue
        if flags & bit:
            out.append(name)
    return ' '.join(out)


def _operand_text(d, op, o, pc):
    parts = []
    f = op.fmt
    if f in ('35c', '45cc'):
        parts.append('{%s}' % ', '.join('v%d' % r for r in o['regs']))
    elif f in ('3rc', '4rcc'):
        parts.append('{v%d .. v%d}' % (o['first'], o['first'] + o['count'] - 1) if o['count'] else '{}')
    for key in ('A', 'B', 'C'):
        if key in o:
            parts.append('v%d' % o[key])
    if 'lit' in o:
        v = o['lit']
        if f == '21h':
            v = v << (48 if op.name.startswith('const-wide') else 16)
        parts.append('%#x' % v if v >= 0 else '-%#x' % -v)
    if 'off' in o:
        parts.append(':L%d' % (pc + o['off']))
    if 'idx' in o:
        k, i = op.ref, o['idx']
        try:
            if k == 'string':
                parts.append('"%s"' % d.string(i).encode('unicode_escape').decode('ascii').replace('"', '\\"'))
            elif k == 'type':
                parts.append(d.type(i))
            elif k == 'field':
                parts.append(d.field_text(i))
            elif k == 'method':
                parts.append(d.method_text(i))
            elif k == 'proto':
                parts.append(d.proto_text(i))
            else:
                parts.append('%s@%d' % (k, i))
        except AsmError as e:
            parts.append('?%s' % e)
    if 'idx2' in o:
        parts.append(d.proto_text(o['idx2']))
    return ', '.join(parts)


def dump_dex(b, want_code, only):
    d = Dex(b)
    for i in range(d.nc):
        cls, flags, sup, ifaces, source, annos, cdata, statics = d.class_def(i)
        name = d.type(cls)
        if only and name != only:
            continue
        print('.class %s %s' % (_flag_words(flags, False), name))
        if sup != NO_INDEX:
            print('.super %s' % d.type(sup))
        for t in d.type_list(ifaces):
            print('.implements %s' % d.type(t))
        sf, inf, dm, vm = d.class_data(cdata)
        for fi, fl in sf + inf:
            _, t, n = d.field(fi)
            print('.field %s %s:%s' % (_flag_words(fl, False), d.string(n), d.type(t)))
        for mi, fl, coff in dm + vm:
            _, p, n = d.method(mi)
            print('.method %s %s%s' % (_flag_words(fl, True), d.string(n), d.proto_text(p)))
            if coff and want_code:
                regs, ins, outs, debug, units, tries = d.code(coff)
                print('    .registers %d    # ins %d, outs %d, %d code units' % (regs, ins, outs, len(units)))
                for pc, op, o, n, payload in walk_code(units):
                    if payload:
                        print('    %5d: %s' % (pc, {0x0100: '.packed-switch', 0x0200: '.sparse-switch', 0x0300: '.array-data'}[units[pc]]),
                              ' '.join('%04x' % u for u in units[pc + 1:pc + min(n, 12)]))
                    elif op is None:
                        print('    %5d: ?? %04x' % (pc, units[pc]))
                    else:
                        print('    %5d: %s %s' % (pc, op.name, _operand_text(d, op, o, pc)))
                for start, count, typed, catch_all in tries:
                    for ti, addr in typed:
                        print('    .catch %s {%d .. %d} :L%d' % (d.type(ti), start, start + count, addr))
                    if catch_all is not None:
                        print('    .catchall {%d .. %d} :L%d' % (start, start + count, catch_all))
            if want_code:
                print('.end method')
        print()


# --------------------------------------------------------------------- C outputs


def c_array(b, name, source):
    """As 32-bit words, so the array is aligned as the code items in it need."""
    pad = (-len(b)) & 3
    words = struct.unpack('<%dI' % ((len(b) + pad) // 4), b + b'\0' * pad)
    lines = ['/* Generated by tools/dexasm.py from %s. Do not edit. */' % source, 'static const unsigned int %s[] = {' % name]
    for i in range(0, len(words), 10):
        lines.append('    ' + ', '.join('0x%08xu' % w for w in words[i:i + 10]) + ',')
    lines.append('};')
    lines.append('static const unsigned int %s_size = %du;' % (name, len(b)))
    return '\n'.join(lines) + '\n'


def c_table():
    fmts = sorted(FORMAT_UNITS)
    lines = ['/* Generated by tools/dexasm.py ctable: the Dalvik instruction set as the assembler knows it. Do not edit. */',
             '/* name, code units (0: not an instruction), format, what its index refers to */']
    lines.append('enum { ' + ', '.join('DVM_F_%s' % f for f in fmts) + ' };')
    lines.append('enum { DVM_R_NONE, DVM_R_STRING, DVM_R_TYPE, DVM_R_FIELD, DVM_R_METHOD, DVM_R_PROTO, DVM_R_CALLSITE, DVM_R_HANDLE };')
    lines.append('static const struct dvm_opinfo { const char *name; unsigned char units, format, ref; } k_dvm_ops[256] = {')
    for code in range(256):
        op = OPS.get(code)
        if op is None:
            lines.append('    /* %02x */ { 0, 0, 0, 0 },' % code)
        else:
            lines.append('    /* %02x */ { "%s", %d, DVM_F_%s, DVM_R_%s },' % (code, op.name, FORMAT_UNITS[op.fmt], op.fmt, (op.ref or 'none').upper()))
    lines.append('};')
    return '\n'.join(lines) + '\n'


# -------------------------------------------------------------------------- main


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    cmd, args = argv[1], argv[2:]
    out, name, want_code, only, files = None, None, False, None, []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-o':
            out = args[i + 1]
            i += 1
        elif a == '--name':
            name = args[i + 1]
            i += 1
        elif a == '--class':
            only = args[i + 1]
            i += 1
        elif a == '--code':
            want_code = True
        else:
            files.append(a)
        i += 1
    try:
        if cmd == 'asm':
            if not out or not files:
                raise AsmError('asm needs -o and at least one .smali file')
            classes = []
            for f in files:
                with open(f, encoding='utf-8') as fh:
                    classes.append(parse_smali(f, fh.read()))
            b = build_dex(classes)
            print(check_dex(b, os.path.basename(out)))
            with open(out, 'wb') as fh:
                fh.write(b)
        elif cmd == 'check':
            for f in files:
                with open(f, 'rb') as fh:
                    print(check_dex(fh.read(), os.path.basename(f)))
        elif cmd == 'dump':
            for f in files:
                with open(f, 'rb') as fh:
                    dump_dex(fh.read(), want_code, only)
        elif cmd == 'carray':
            if not out or not name or len(files) != 1:
                raise AsmError('carray needs -o, --name and one .dex file')
            with open(files[0], 'rb') as fh:
                text = c_array(fh.read(), name, os.path.basename(files[0]))
            with open(out, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(text)
        elif cmd == 'ctable':
            if not out:
                raise AsmError('ctable needs -o')
            with open(out, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(c_table())
        else:
            raise AsmError('unknown command %s' % cmd)
    except AsmError as e:
        print('dexasm: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
