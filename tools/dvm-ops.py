#!/usr/bin/env python3
"""
Makes the arithmetic part of the Dalvik runtime's tests.

    python tools/dvm-ops.py OUTDIR

Writes OUTDIR/Ops.smali, a class t.Ops with one static method for each
arithmetic, conversion, comparison and conditional-branch instruction in each
of its forms (three registers, two registers, with a literal), and
OUTDIR/hk_dvm_ops_cases.h, a table of calls to them with what each must
return.

The expected values are worked out here, in Python, from what the Java
language says each operation does -- wrapping integers, division that rounds
toward zero, shift counts masked, single-precision floats, conversions that
saturate -- and not by the interpreter under test or by C doing the same
thing the interpreter does. A float or double travels as its bits.
"""
import math
import os
import struct
import sys

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
IMIN, IMAX = -0x80000000, 0x7FFFFFFF
LMIN, LMAX = -0x8000000000000000, 0x7FFFFFFFFFFFFFFF
INF, NAN = float('inf'), float('nan')


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def s64(x):
    x &= M64
    return x - (1 << 64) if x & (1 << 63) else x


def f32(x):
    """A double rounded to the nearest float, as Java's (float) does."""
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(INF, x)


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def dbits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


class Throws(Exception):
    pass


def idiv(a, b):
    if b == 0:
        raise Throws('java/lang/ArithmeticException')
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def irem(a, b):
    return a - idiv(a, b) * b


def fdiv(a, b):
    if b == 0:
        if a == 0 or a != a:
            return NAN
        return math.copysign(INF, a) * math.copysign(1.0, b)
    return a / b


def frem(a, b):
    try:
        return math.fmod(a, b)
    except ValueError:
        return NAN


def to_int(x, lo, hi):
    """float or double to int or long: NaN is zero, out of range is the nearest end, the rest loses its fraction."""
    if x != x:
        return 0
    if x >= hi:
        return hi
    if x <= lo:
        return lo
    return int(x)


INT_BIN = {
    'add': lambda a, b: a + b, 'sub': lambda a, b: a - b, 'mul': lambda a, b: a * b, 'div': idiv, 'rem': irem,
    'and': lambda a, b: a & b, 'or': lambda a, b: a | b, 'xor': lambda a, b: a ^ b,
}
REAL_BIN = {'add': lambda a, b: a + b, 'sub': lambda a, b: a - b, 'mul': lambda a, b: a * b, 'div': fdiv, 'rem': frem}


def int_op(name, a, b, bits):
    wrap, mask = (s32, 31) if bits == 32 else (s64, 63)
    if name == 'shl':
        return wrap(a << (b & mask))
    if name == 'shr':
        return wrap(a >> (b & mask))
    if name == 'ushr':
        return wrap((a & (M32 if bits == 32 else M64)) >> (b & mask))
    return wrap(INT_BIN[name](a, b))


INT_PAIRS = [(7, 3), (-7, 3), (7, -3), (-7, -3), (0, 5), (IMIN, -1), (IMIN, 1), (IMAX, IMAX), (IMAX, 1), (0x12345678, 33), (-1, 31), (1, 32), (5, 0)]
LONG_PAIRS = [(7, 3), (-7, 3), (7, -3), (-7, -3), (0, 5), (LMIN, -1), (LMIN, 1), (LMAX, LMAX), (LMAX, 1), (0x123456789ABCDEF0, 0x0FEDCBA987654321),
              (-1, 0x100000000), (0x100000000, 0x100000000), (5, 0)]
LONG_SHIFTS = [(1, 0), (1, 1), (1, 63), (1, 64), (1, 65), (-1, 1), (-1, 63), (LMIN, 1), (0x123456789ABCDEF0, 36), (0x123456789ABCDEF0, -4), (-0x123456789ABCDEF0, 33)]
FLOAT_PAIRS = [(1.5, 2.25), (-7.5, 2.0), (1e30, 1e30), (0.0, 0.0), (1.0, 0.0), (-1.0, 0.0), (NAN, 1.0), (INF, INF), (INF, 2.0), (5.5, -2.0),
               (1.4e-45, 0.5), (3.4028235e38, 3.4028235e38), (0.1, 0.2)]
DOUBLE_PAIRS = [(1.5, 2.25), (-7.5, 2.0), (1e300, 1e300), (0.0, 0.0), (1.0, 0.0), (-1.0, 0.0), (NAN, 1.0), (INF, INF), (INF, 2.0), (5.5, -2.0),
                (4.9e-324, 0.5), (1.7976931348623157e308, 1.7976931348623157e308), (0.1, 0.2)]
INTS = [0, 1, -1, 127, 128, -129, 65535, 65536, 0x12345678, -0x12345678, IMAX, IMIN]
LONGS = [0, 1, -1, 0x7FFFFFFF, 0x80000000, -0x80000001, 123456789012345, (1 << 24) + 1, 1 << 62, LMAX - 0x3FF, LMIN, 0x123456789ABCDEF0]
FLOATS = [0.0, -0.0, 1.0, -1.5, 2.5, 0.99, -0.99, 2147483520.0, 3e9, -3e9, 1e19, -1e19, 3.4028235e38, 1.4e-45, INF, -INF, NAN]
DOUBLES = [0.0, -0.0, 1.0, -1.5, 2.5, 0.99, -0.99, 2147483647.0, 2147483648.5, -2147483649.5, 9.3e18, -9.3e18, 1e300, 4.9e-324, 3.5e38, 1.0000000001, INF,
           -INF, NAN]

smali = []
cases = []


def method(name, sig, registers, body):
    smali.append('.method public static %s%s' % (name, sig))
    smali.append('    .registers %d' % registers)
    smali.extend('    ' + line if not line.startswith(':') else '    ' + line for line in body)
    smali.append('.end method')
    smali.append('')


def enc(kind, v):
    """A value as the table holds it: an integer, or the bits of a float or double."""
    if kind == 'F':
        return fbits(v)
    if kind == 'D':
        return dbits(v)
    return v


def case(name, sig, args, fn):
    kinds = [k for k in sig[1:sig.index(')')]]
    ret = sig[-1]
    try:
        want = fn(*args)
        throws = None
    except Throws as e:
        want, throws = 0, str(e)
    is_nan = ret in 'FD' and want != want
    cases.append((name, sig, [enc(k, a) for k, a in zip(kinds, args)], 0 if is_nan else enc(ret, want), is_nan, throws))


def build():
    # ---- binary operations: three registers, two registers
    for word, kind, bits, pairs in (('int', 'I', 32, INT_PAIRS), ('long', 'J', 64, LONG_PAIRS)):
        wide = kind == 'J'
        for op in ('add', 'sub', 'mul', 'div', 'rem', 'and', 'or', 'xor', 'shl', 'shr', 'ushr'):
            shift = op in ('shl', 'shr', 'ushr')
            sig = '(%s%s)%s' % (kind, 'I' if shift else kind, kind)
            second = 'p2' if wide else 'p1'
            ins = (2 if wide else 1) + (1 if shift or not wide else 2)
            ret = 'return-wide' if wide else 'return'
            name = '%s_%s' % (op, word)
            method(name, sig, ins + (2 if wide else 1), ['%s-%s v0, p0, %s' % (op, word, second), '%s v0' % ret])
            method(name + '_2addr', sig, ins, ['%s-%s/2addr p0, %s' % (op, word, second), '%s p0' % ret])
            for a, b in (LONG_SHIFTS if wide and shift else pairs):
                if wide and shift:
                    b = s32(b)
                for n in (name, name + '_2addr'):
                    case(n, sig, (a, b), lambda x, y, op=op, bits=bits: int_op(op, x, y, bits))
    for word, kind, pairs, rnd in (('float', 'F', FLOAT_PAIRS, f32), ('double', 'D', DOUBLE_PAIRS, float)):
        wide = kind == 'D'
        for op in ('add', 'sub', 'mul', 'div', 'rem'):
            sig = '(%s%s)%s' % (kind, kind, kind)
            second = 'p2' if wide else 'p1'
            ins = 4 if wide else 2
            ret = 'return-wide' if wide else 'return'
            name = '%s_%s' % (op, word)
            method(name, sig, ins + (2 if wide else 1), ['%s-%s v0, p0, %s' % (op, word, second), '%s v0' % ret])
            method(name + '_2addr', sig, ins, ['%s-%s/2addr p0, %s' % (op, word, second), '%s p0' % ret])
            for a, b in pairs:
                a, b = rnd(a), rnd(b)
                for n in (name, name + '_2addr'):
                    case(n, sig, (a, b), lambda x, y, op=op, rnd=rnd: rnd(REAL_BIN[op](x, y)))

    # ---- an int and a literal
    lit16 = [('add', 'add-int/lit16'), ('rsub', 'rsub-int'), ('mul', 'mul-int/lit16'), ('div', 'div-int/lit16'), ('rem', 'rem-int/lit16'),
             ('and', 'and-int/lit16'), ('or', 'or-int/lit16'), ('xor', 'xor-int/lit16')]
    lit8 = [('add', 'add-int/lit8'), ('rsub', 'rsub-int/lit8'), ('mul', 'mul-int/lit8'), ('div', 'div-int/lit8'), ('rem', 'rem-int/lit8'),
            ('and', 'and-int/lit8'), ('or', 'or-int/lit8'), ('xor', 'xor-int/lit8'), ('shl', 'shl-int/lit8'), ('shr', 'shr-int/lit8'),
            ('ushr', 'ushr-int/lit8')]
    for table, lits, tag in ((lit16, (0x1234, -0x7f00, 0, -1), 'lit16'), (lit8, (7, -128, 0, 127, 33), 'lit8')):
        for op, mnemonic in table:
            for lit in lits:
                name = '%s_%s_%s' % (op, tag, ('m%d' % -lit) if lit < 0 else str(lit))
                method(name, '(I)I', 2, ['%s v0, p0, %d' % (mnemonic, lit), 'return v0'])
                for a in (0, 1, -1, 100, -100, IMAX, IMIN, 0x12345678):
                    if op == 'rsub':
                        case(name, '(I)I', (a,), lambda x, lit=lit: s32(lit - x))
                    else:
                        case(name, '(I)I', (a,), lambda x, op=op, lit=lit: int_op(op, x, lit, 32))

    # ---- one register in, one out
    def neg_real(x):
        return -x

    unary = [
        ('neg-int', 'I', 'I', INTS, lambda a: s32(-a)), ('not-int', 'I', 'I', INTS, lambda a: s32(~a)),
        ('neg-long', 'J', 'J', LONGS, lambda a: s64(-a)), ('not-long', 'J', 'J', LONGS, lambda a: s64(~a)),
        ('neg-float', 'F', 'F', FLOATS, neg_real), ('neg-double', 'D', 'D', DOUBLES, neg_real),
        ('int-to-long', 'I', 'J', INTS, lambda a: a), ('int-to-float', 'I', 'F', INTS, lambda a: f32(float(a))),
        ('int-to-double', 'I', 'D', INTS, lambda a: float(a)),
        ('long-to-int', 'J', 'I', LONGS, s32), ('long-to-float', 'J', 'F', LONGS, lambda a: f32(float(a))),
        ('long-to-double', 'J', 'D', LONGS + [(1 << 53) + 1, -((1 << 53) + 3)], lambda a: float(a)),
        ('float-to-int', 'F', 'I', FLOATS, lambda a: to_int(a, IMIN, IMAX)), ('float-to-long', 'F', 'J', FLOATS, lambda a: to_int(a, LMIN, LMAX)),
        ('float-to-double', 'F', 'D', FLOATS, lambda a: a),
        ('double-to-int', 'D', 'I', DOUBLES, lambda a: to_int(a, IMIN, IMAX)), ('double-to-long', 'D', 'J', DOUBLES, lambda a: to_int(a, LMIN, LMAX)),
        ('double-to-float', 'D', 'F', DOUBLES, f32),
        ('int-to-byte', 'I', 'B', INTS, lambda a: ((a & 0xFF) ^ 0x80) - 0x80), ('int-to-char', 'I', 'C', INTS, lambda a: a & 0xFFFF),
        ('int-to-short', 'I', 'S', INTS, lambda a: ((a & 0xFFFF) ^ 0x8000) - 0x8000),
    ]
    for mnemonic, src, dst, values, fn in unary:
        name = mnemonic.replace('-', '_')
        sig = '(%s)%s' % (src, dst)
        regs = (2 if dst in 'JD' else 1) + (2 if src in 'JD' else 1)
        method(name, sig, regs, ['%s v0, p0' % mnemonic, '%s v0' % ('return-wide' if dst in 'JD' else 'return')])
        for a in values:
            if src == 'F':
                a = f32(a)
            case(name, sig, (a,), fn)
    # The same in place: the two-register form writes over what it reads, and a long written over an int must not be read half-written.
    method('int_to_long_in_place', '(I)J', 3, ['move v0, p0', 'int-to-long v0, v0', 'return-wide v0'])
    method('long_to_int_in_place', '(J)I', 2, ['long-to-int p0, p0', 'return p0'])
    for a in INTS:
        case('int_to_long_in_place', '(I)J', (a,), lambda x: x)
    for a in LONGS:
        case('long_to_int_in_place', '(J)I', (a,), s32)

    # ---- comparisons
    def cmp_real(a, b, nan):
        if a != a or b != b:
            return nan
        return -1 if a < b else 1 if a > b else 0

    for mnemonic, kind, pairs, nan in (('cmpl-float', 'F', FLOAT_PAIRS, -1), ('cmpg-float', 'F', FLOAT_PAIRS, 1), ('cmpl-double', 'D', DOUBLE_PAIRS, -1),
                                       ('cmpg-double', 'D', DOUBLE_PAIRS, 1)):
        wide = kind == 'D'
        name = mnemonic.replace('-', '_')
        sig = '(%s%s)I' % (kind, kind)
        method(name, sig, 5 if wide else 3, ['%s v0, p0, %s' % (mnemonic, 'p2' if wide else 'p1'), 'return v0'])
        for a, b in pairs + [(2.0, 1.0), (1.0, NAN)]:
            if kind == 'F':
                a, b = f32(a), f32(b)
            case(name, sig, (a, b), lambda x, y, nan=nan: cmp_real(x, y, nan))
    method('cmp_long', '(JJ)I', 5, ['cmp-long v0, p0, p2', 'return v0'])
    for a, b in LONG_PAIRS + [(1, 0x100000000), (0x100000000, 1), (-0x100000000, 1), (LMIN, LMAX)]:
        case('cmp_long', '(JJ)I', (a, b), lambda x, y: -1 if x < y else 1 if x > y else 0)

    # ---- conditional branches
    tests = {'eq': lambda a, b: a == b, 'ne': lambda a, b: a != b, 'lt': lambda a, b: a < b, 'ge': lambda a, b: a >= b, 'gt': lambda a, b: a > b,
             'le': lambda a, b: a <= b}
    for cond, fn in tests.items():
        body = ['if-%s p0, p1, :yes' % cond, 'const/4 v0, 0x0', 'return v0', ':yes', 'const/4 v0, 0x1', 'return v0']
        method('if_%s' % cond, '(II)I', 3, body)
        for a, b in ((1, 2), (2, 1), (2, 2), (-1, 1), (IMIN, IMAX), (IMAX, IMIN), (-5, -5)):
            case('if_%s' % cond, '(II)I', (a, b), lambda x, y, fn=fn: 1 if fn(x, y) else 0)
        body = ['if-%sz p0, :yes' % cond, 'const/4 v0, 0x0', 'return v0', ':yes', 'const/4 v0, 0x1', 'return v0']
        method('if_%sz' % cond, '(I)I', 2, body)
        for a in (0, 1, -1, IMIN, IMAX):
            case('if_%sz' % cond, '(I)I', (a,), lambda x, fn=fn: 1 if fn(x, 0) else 0)


def c_value(v):
    return '(long long)0x%016xULL' % (v & M64)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    out = argv[1]
    build()
    text = ['# Generated by tools/dvm-ops.py: one method for each arithmetic instruction in each of its forms.', '.class public final Lt/Ops;',
            '.super Ljava/lang/Object;', ''] + smali
    header = ['/* Generated by tools/dvm-ops.py. Do not edit. What each method of t.Ops must return, worked out from the Java language\'s rules. */',
              'typedef struct ops_case {', '    const char *name, *sig;', '    long long a[2];', '    long long want;                    /* an integer, or the bits of a float or double */',
              '    int want_nan;', '    const char *throws;                /* the exception it must throw instead, or NULL */', '} ops_case;', '',
              'static const ops_case k_ops_cases[] = {']
    for name, sig, args, want, is_nan, throws in cases:
        a = args + [0] * (2 - len(args))
        header.append('    { "%s", "%s", { %s, %s }, %s, %d, %s },' % (name, sig, c_value(a[0]), c_value(a[1]), c_value(want), 1 if is_nan else 0,
                                                                     '"%s"' % throws if throws else 'NULL'))
    header.append('};')
    for path, lines in ((os.path.join(out, 'Ops.smali'), text), (os.path.join(out, 'hk_dvm_ops_cases.h'), header)):
        body = '\n'.join(lines) + '\n'
        old = None
        if os.path.exists(path):
            with open(path, encoding='utf-8') as fh:
                old = fh.read()
        if old != body:
            with open(path, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(body)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
