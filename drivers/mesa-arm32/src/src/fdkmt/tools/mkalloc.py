"""Tao private driver data cho D3DKMTCreateAllocation2 cua qcdxkm8996.

Layout theo D:\\Git\\Driver\\PHAN-TICH-SAU-RENDER-PATCH.md muc 5 (DxgkDdiCreateAllocation):
  header 0x130 byte + (N-1) x 0x4C descriptor; dword[0] = 0x130; dword[0x0E] = N;
  dword[1] = CRC: seed 1, poly 0x04C11DB7, tinh tren toan blob voi cac dword runtime
  {1, 8..0xD, 0x2E..0x31} = 0.

  python3 mkalloc.py OUT.bin [bytes] [--width W --height H --format F --pitch P --flags X]
"""
import argparse
import struct

POLY = 0x04C11DB7
RUNTIME = [1, 8, 9, 0xA, 0xB, 0xC, 0xD, 0x2E, 0x2F, 0x30, 0x31]


def alloc_crc(words):
    w = list(words)
    for i in RUNTIME:
        w[i] = 0
    c = 1
    for x in w:
        mask = 0xFFFFFFFF if (c >> 31) else 0
        c = ((mask & POLY) ^ ((c << 1) & 0xFFFFFFFF) ^ x) & 0xFFFFFFFF
    return c


def build(nbytes, width=None, height=1, fmt=0, pitch=None, alloc_flags=0, n=1):
    size = 0x130 + (n - 1) * 0x4C
    w = [0] * (size // 4)
    w[0x00] = 0x130
    w[0x04] = (nbytes + 0xFFF) & ~0xFFF
    w[0x0E] = n
    w[0x0F] = alloc_flags
    w[0x11] = height
    w[0x12] = width if width is not None else nbytes
    w[0x13] = fmt
    w[0x14] = pitch if pitch is not None else nbytes
    w[1] = alloc_crc(w)
    return struct.pack('<%dI' % len(w), *w)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('bytes', nargs='?', type=lambda v: int(v, 0), default=0x10000)
    ap.add_argument('--width', type=lambda v: int(v, 0))
    ap.add_argument('--height', type=lambda v: int(v, 0), default=1)
    ap.add_argument('--format', type=lambda v: int(v, 0), default=0)
    ap.add_argument('--pitch', type=lambda v: int(v, 0))
    ap.add_argument('--flags', type=lambda v: int(v, 0), default=0)
    a = ap.parse_args()
    blob = build(a.bytes, a.width, a.height, a.format, a.pitch, a.flags)
    open(a.out, 'wb').write(blob)
    print('%s: %d byte, crc=0x%08x' % (a.out, len(blob), struct.unpack_from('<I', blob, 4)[0]))
