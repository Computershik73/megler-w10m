"""Makes a small APK for the tests: a binary AndroidManifest.xml written here, native libraries and assets.

    make-test-apk.py OUT.apk --package NAME --label TEXT --lib-name NAME [--lib FILE]... [--asset NAME=TEXT]...

The manifest describes a NativeActivity app (android:hasCode="false", android.app.lib_name), in
the same binary XML format aapt writes, so the core's manifest reader is tested against the
format and not against itself. Libraries are stored uncompressed, as an APK with
extractNativeLibs="false" has them; assets are deflated.
"""
import argparse
import os
import struct
import zipfile

ANDROID_NS = 'http://schemas.android.com/apk/res/android'
# attribute name -> its resource id (android.R.attr)
ATTR_IDS = {
    'versionCode': 0x0101021b, 'versionName': 0x0101021c, 'minSdkVersion': 0x0101020c, 'targetSdkVersion': 0x01010270,
    'label': 0x01010001, 'hasCode': 0x0101000c, 'name': 0x01010003, 'value': 0x01010024,
}
TYPE_STRING, TYPE_INT, TYPE_BOOL = 0x03, 0x10, 0x12


class Manifest:
    def __init__(self):
        # Attribute names with resource ids come first: the resource map is indexed like the string pool.
        self.strings = list(ATTR_IDS.keys())
        self.body = b''

    def s(self, text):
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text)

    def start(self, tag, attrs):
        """attrs: (name, kind, value) with kind 's' string, 'i' int, 'b' bool; a name without a resource id has no namespace."""
        out = b''
        for name, kind, value in attrs:
            ns = self.s(ANDROID_NS) if name in ATTR_IDS else 0xFFFFFFFF
            if kind == 's':
                raw, dtype, data = self.s(value), TYPE_STRING, self.s(value)
            elif kind == 'i':
                raw, dtype, data = 0xFFFFFFFF, TYPE_INT, value
            else:
                raw, dtype, data = 0xFFFFFFFF, TYPE_BOOL, 0xFFFFFFFF if value else 0
            out += struct.pack('<IIIHBBI', ns, self.s(name), raw, 8, 0, dtype, data)
        chunk = struct.pack('<IIIIHHHHHH', 1, 0xFFFFFFFF, 0xFFFFFFFF, self.s(tag), 20, 20, len(attrs), 0, 0, 0) + out
        self.body += struct.pack('<HHI', 0x0102, 16, 8 + len(chunk)) + chunk

    def end(self, tag):
        chunk = struct.pack('<IIII', 1, 0xFFFFFFFF, 0xFFFFFFFF, self.s(tag))
        self.body += struct.pack('<HHI', 0x0103, 16, 8 + len(chunk)) + chunk

    def build(self):
        prefix, uri = self.s('android'), self.s(ANDROID_NS)
        ns_start = struct.pack('<HHIIIII', 0x0100, 16, 24, 1, 0xFFFFFFFF, prefix, uri)
        ns_end = struct.pack('<HHIIIII', 0x0101, 16, 24, 1, 0xFFFFFFFF, prefix, uri)
        # The string pool, UTF-16.
        data, offsets = b'', []
        for text in self.strings:
            offsets.append(len(data))
            units = text.encode('utf-16-le')
            data += struct.pack('<H', len(units) // 2) + units + b'\0\0'
        data += b'\0' * (-len(data) % 4)
        header = 28
        strings_start = header + 4 * len(self.strings)
        pool = struct.pack('<HHIIIIII', 0x0001, header, strings_start + len(data), len(self.strings), 0, 0, strings_start, 0)
        pool += b''.join(struct.pack('<I', o) for o in offsets) + data
        ids = b''.join(struct.pack('<I', ATTR_IDS[name]) for name in self.strings[:len(ATTR_IDS)])
        resmap = struct.pack('<HHI', 0x0180, 8, 8 + len(ids)) + ids
        content = pool + resmap + ns_start + self.body + ns_end
        return struct.pack('<HHI', 0x0003, 8, 8 + len(content)) + content


def main():
    p = argparse.ArgumentParser()
    p.add_argument('out')
    p.add_argument('--package', required=True)
    p.add_argument('--label', default='Test')
    p.add_argument('--lib-name', required=True)
    p.add_argument('--abi', default='armeabi-v7a')
    p.add_argument('--lib', action='append', default=[])
    p.add_argument('--asset', action='append', default=[])
    a = p.parse_args()

    m = Manifest()
    m.start('manifest', [('versionCode', 'i', 7), ('versionName', 's', '1.2'), ('package', 's', a.package)])
    m.start('uses-sdk', [('minSdkVersion', 'i', 16), ('targetSdkVersion', 'i', 28)])
    m.end('uses-sdk')
    m.start('application', [('label', 's', a.label), ('hasCode', 'b', False)])
    m.start('activity', [('name', 's', 'android.app.NativeActivity')])
    m.start('meta-data', [('name', 's', 'android.app.lib_name'), ('value', 's', a.lib_name)])
    m.end('meta-data')
    m.start('intent-filter', [])
    m.start('action', [('name', 's', 'android.intent.action.MAIN')])
    m.end('action')
    m.start('category', [('name', 's', 'android.intent.category.LAUNCHER')])
    m.end('category')
    m.end('intent-filter')
    m.end('activity')
    m.end('application')
    m.end('manifest')

    if os.path.exists(a.out):
        os.remove(a.out)
    with zipfile.ZipFile(a.out, 'w') as z:
        z.writestr(zipfile.ZipInfo('AndroidManifest.xml'), m.build(), zipfile.ZIP_DEFLATED)
        for lib in a.lib:
            z.write(lib, 'lib/%s/%s' % (a.abi, os.path.basename(lib)), zipfile.ZIP_STORED)
        for asset in a.asset:
            name, text = asset.split('=', 1)
            z.writestr(zipfile.ZipInfo('assets/' + name), text, zipfile.ZIP_DEFLATED)


main()
