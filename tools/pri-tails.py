"""Where the package name sits in the data of a resources.pri's schema checksum (core/src/appx/hk_appx_pri.c).

    python tools/pri-tails.py <template.pri> <probe.pri> <placeholder> <index>

The two files were made by makepri for the same files and manifest but the identity name: <placeholder> in the
template, and in the probe the same with the character at <index> one bit apart ('0' made '1'). The checksum (CRC-32,
in "[def_hnamesx]", 24 bytes on) covers the name twice, lower-cased and in UTF-16; the difference of the two checksums is
the CRC of that character's difference followed by as many zero bytes as come after it, at both places. Prints the two
tails, the bytes after the end of the name at each place: "<tail1> <tail2>". Ends 1 when they are not found.
"""
import struct
import sys
import zlib


def checksum(d):
    i = d.find(b"[def_hnamesx]")
    if i < 0:
        sys.exit("no [def_hnamesx] in the file")
    return struct.unpack("<I", d[i + 24:i + 28])[0]


def crc_of_difference(delta, after):
    n = len(delta) + after
    return zlib.crc32(delta + b"\0" * after) ^ zlib.crc32(b"\0" * n)


def main():
    template, probe, placeholder, index = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read(), sys.argv[3], int(sys.argv[4])
    diff = checksum(template) ^ checksum(probe)
    delta = bytes([ord("0") ^ ord("1"), 0])
    limit = 4000
    by_value = {}
    for after in range(limit):
        by_value.setdefault(crc_of_difference(delta, after), []).append(after)
    for a in range(limit):
        for b in by_value.get(crc_of_difference(delta, a) ^ diff, []):
            if b > a:
                past = (len(placeholder) - index - 1) * 2
                print(a - past, b - past)
                return 0
    print("the name's places were not found", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
