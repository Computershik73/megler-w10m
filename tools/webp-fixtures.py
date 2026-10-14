"""
Writes core/tests/hk_webp_fixtures.inc: five small WebP files as C arrays, for the test of the core's WebP decoding
(core/tests/hk_gfx_natives_tests.c). The file is kept in the repository, so the tests need neither this script nor what it
needs; it is for when the pictures are to change.

    python tools/webp-fixtures.py

Needs ffmpeg with libwebp on the PATH (it is the encoder; this repository has only a decoder). The pictures are 32 x 32:

    lossless      four quadrants -- opaque red, fully transparent, opaque blue, half-transparent white -- kept exactly
    lossy         four opaque quadrants, red, green, blue, white, at quality 90: the colours come back nearly
    lossy_alpha   the first picture again, lossy with an alpha plane
    anim          two frames, lossless, the first all orange (255, 128, 0), the second all teal
    anim_part     an animation put together here: one frame of 16 x 16 opaque magenta at (8, 8) on a 32 x 32 canvas,
                  for the case of a frame that does not fill its canvas
"""
import os, struct, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'core', 'tests', 'hk_webp_fixtures.inc')


def png(path, width, height, pixel):
    rows = b''
    for y in range(height):
        rows += b'\0' + b''.join(bytes(pixel(x, y)) for x in range(width))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(bytes((137, 80, 78, 71, 13, 10, 26, 10)) + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9))
                + chunk(b'IEND', b''))


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y'] + list(args), check=True)


def chunks(data):
    at = 12
    while at + 8 <= len(data):
        n, = struct.unpack_from('<I', data, at + 4)
        yield data[at:at + 4], data[at + 8:at + 8 + n]
        at += 8 + n + (n & 1)


def chunk(kind, payload):
    return kind + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')


def u24(v):
    return struct.pack('<I', v)[:3]


def main():
    d = tempfile.mkdtemp()
    p = lambda name: os.path.join(d, name)
    with_alpha = lambda x, y: (255, 0, 0, 255) if x < 16 and y < 16 else (0, 0, 0, 0) if y < 16 else (0, 0, 255, 255) if x < 16 else (255, 255, 255, 128)
    opaque = lambda x, y: (255, 0, 0, 255) if x < 16 and y < 16 else (0, 255, 0, 255) if y < 16 else (0, 0, 255, 255) if x < 16 else (255, 255, 255, 255)
    png(p('alpha.png'), 32, 32, with_alpha)
    png(p('opaque.png'), 32, 32, opaque)
    png(p('f1.png'), 32, 32, lambda x, y: (255, 128, 0, 255))
    png(p('f2.png'), 32, 32, lambda x, y: (0, 128, 128, 255))
    png(p('part.png'), 16, 16, lambda x, y: (255, 0, 255, 255))
    ffmpeg('-i', p('alpha.png'), '-c:v', 'libwebp', '-lossless', '1', '-pix_fmt', 'bgra', p('lossless.webp'))
    ffmpeg('-i', p('opaque.png'), '-c:v', 'libwebp', '-lossless', '0', '-q:v', '90', '-pix_fmt', 'yuv420p', p('lossy.webp'))
    ffmpeg('-i', p('alpha.png'), '-c:v', 'libwebp', '-lossless', '0', '-q:v', '90', '-pix_fmt', 'yuva420p', p('lossy_alpha.webp'))
    ffmpeg('-framerate', '2', '-i', p('f%d.png'), '-c:v', 'libwebp_anim', '-lossless', '1', '-loop', '0', p('anim.webp'))
    ffmpeg('-i', p('part.png'), '-c:v', 'libwebp', '-lossless', '1', '-pix_fmt', 'bgra', p('part.webp'))
    files = {name: open(p(name + '.webp'), 'rb').read() for name in ('lossless', 'lossy', 'lossy_alpha', 'anim')}
    # The partial frame: the still's picture chunk inside an ANMF chunk, behind the headers an animation has.
    part = [c for c in chunks(open(p('part.webp'), 'rb').read()) if c[0] in (b'VP8L', b'VP8 ', b'ALPH')]
    frame = u24(8 // 2) + u24(8 // 2) + u24(16 - 1) + u24(16 - 1) + u24(100) + b'\0' + b''.join(chunk(k, v) for k, v in part)
    body = b'WEBP' + chunk(b'VP8X', bytes((0x02 | 0x10, 0, 0, 0)) + u24(32 - 1) + u24(32 - 1)) + chunk(b'ANIM', struct.pack('<IH', 0, 0)) + chunk(b'ANMF', frame)
    files['anim_part'] = b'RIFF' + struct.pack('<I', len(body)) + body
    kinds = {name: [k.decode('latin-1') for k, _ in chunks(data)] for name, data in files.items()}
    with open(OUT, 'w', newline='\n') as f:
        f.write('/* Written by tools/webp-fixtures.py, which says what each picture is. Not to be edited. */\n')
        for name, data in files.items():
            f.write('/* %s: %d bytes; chunks %s */\n' % (name, len(data), ' '.join(kinds[name])))
            f.write('static const u8 k_webp_%s[] = {\n' % name)
            for i in range(0, len(data), 24):
                f.write('    ' + ' '.join('%d,' % b for b in data[i:i + 24]) + '\n')
            f.write('};\n')
    for name, data in files.items():
        print(name, len(data), 'bytes;', ' '.join(kinds[name]))
    print('written', OUT)


if __name__ == '__main__':
    sys.exit(main())
