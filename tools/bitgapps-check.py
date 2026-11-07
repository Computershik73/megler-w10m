"""Check the Google APKs staged for a package (its bitgapps folder) against the processor the package is for.

    python bitgapps-check.py ABI FOLDER

Each APK of FOLDER must be the app its name says (PrebuiltGmsCore.apk Google Play services, Phonesky.apk Google Play
Store, GoogleServicesFramework.apk Google Services Framework, MicroGGmsCore.apk microG's Google Play services), and its
native code, if it has any, must be ABI's, whole: as many libraries of ABI as of any other processor it has code for.
Megler runs the code of its own processor alone (docs/DESKTOP.md), and Google builds Google Play services in a variant for
each processor whose libraries for a second one are a few of them only: the arm64-v8a variant carries 8 armeabi-v7a
libraries beside its 40 arm64-v8a ones, the x86_64 variant 8 x86 ones (libgmscore.so and libleveldbjni.so not among them).
A phone's package once carried the arm64-v8a variant, and Google Play services' own libraries were "not found" there.

Prints each APK's package, version and native code, and exits 1 on the first that does not fit. Reads the APKs only.
"""

import struct
import sys
import zipfile
from pathlib import Path

PACKAGES = {
    "PrebuiltGmsCore.apk": "com.google.android.gms",
    "Phonesky.apk": "com.android.vending",
    "GoogleServicesFramework.apk": "com.google.android.gsf",
    "MicroGGmsCore.apk": "com.google.android.gms",
}

# The android: attributes of <manifest> by resource id, for a manifest whose attribute names are left out.
VERSION_CODE, VERSION_NAME = 0x0101021B, 0x0101021C


def manifest_facts(data: bytes) -> dict:
    """package, versionCode and versionName of a binary AndroidManifest.xml (Android's AXML)."""
    strings, resource_ids = [], []
    pos = 8
    while pos + 8 <= len(data):
        kind, header, size = struct.unpack_from("<HHI", data, pos)
        if size < 8:
            break
        if kind == 0x0001:  # the string pool
            count, _, flags, start, _ = struct.unpack_from("<IIIII", data, pos + 8)
            utf8 = flags & 0x100
            for i in range(count):
                at = pos + start + struct.unpack_from("<I", data, pos + header + 4 * i)[0]
                if utf8:
                    n = data[at]
                    at += 2 if n & 0x80 else 1
                    n = data[at]
                    if n & 0x80:
                        n = (n & 0x7F) << 8 | data[at + 1]
                        at += 1
                    strings.append(data[at + 1:at + 1 + n].decode("utf-8", "replace"))
                else:
                    n = struct.unpack_from("<H", data, at)[0]
                    if n & 0x8000:
                        n = (n & 0x7FFF) << 16 | struct.unpack_from("<H", data, at + 2)[0]
                        at += 2
                    strings.append(data[at + 2:at + 2 + 2 * n].decode("utf-16-le", "replace"))
        elif kind == 0x0180:  # the attributes' resource ids, by string index
            resource_ids = list(struct.unpack_from(f"<{(size - header) // 4}I", data, pos + header))
        elif kind == 0x0102:  # an element's start: the first is <manifest>
            name = struct.unpack_from("<I", data, pos + 20)[0]
            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", data, pos + 24)
            if strings[name] != "manifest":
                raise ValueError("the first element is not <manifest>")
            facts = {}
            for k in range(attr_count):
                at = pos + header + attr_start + k * attr_size
                _, aname, raw, _, _, dtype, value = struct.unpack_from("<IIIHBBI", data, at)
                rid = resource_ids[aname] if aname < len(resource_ids) else 0
                key = strings[aname] if aname < len(strings) else ""
                if key == "package":
                    facts["package"] = strings[raw]
                elif rid == VERSION_CODE or key == "versionCode":
                    facts["versionCode"] = value
                elif rid == VERSION_NAME or key == "versionName":
                    facts["versionName"] = strings[raw] if raw != 0xFFFFFFFF else (strings[value] if dtype == 3 else "?")
            return facts
        pos += size
    raise ValueError("no <manifest> element")


def check(abi: str, apk: Path) -> str:
    """One line on the APK, or ValueError when it is not for a package of `abi`."""
    with zipfile.ZipFile(apk) as z:
        facts = manifest_facts(z.read("AndroidManifest.xml"))
        libraries = {}
        for name in z.namelist():
            parts = name.split("/")
            if len(parts) == 3 and parts[0] == "lib" and parts[2].endswith(".so"):
                libraries.setdefault(parts[1], set()).add(parts[2])
    want = PACKAGES.get(apk.name)
    if want and facts.get("package") != want:
        raise ValueError(f"{apk.name} is {facts.get('package', '?')}, not {want}")
    line = f"{apk.name}: {facts.get('package', '?')} {facts.get('versionName', '?')}, version code {facts.get('versionCode', '?')}, "
    if not libraries:
        return line + "no native code"
    most = max(len(names) for names in libraries.values())
    ours = len(libraries.get(abi, ()))
    others = ", ".join(f"{a} {len(n)}" for a, n in sorted(libraries.items()) if a != abi)
    line += f"native code: {abi} {ours} libraries" + (f" (besides: {others})" if others else "")
    if ours == 0:
        raise ValueError(f"{line} -- it has no {abi} code: give the APK of {abi} (or one of every processor)")
    if ours < most:
        biggest = max(libraries, key=lambda a: len(libraries[a]))
        raise ValueError(f"{line} -- {ours} {abi} libraries where it has {most} of {biggest}: this is the {biggest} variant, whose "
                         f"{abi} code is a part only; give the APK of {abi}")
    return line


def main(abi: str, folder: Path) -> int:
    failed = False
    for apk in sorted(folder.glob("*.apk")):
        try:
            print(f"    {check(abi, apk)}")
        except (ValueError, KeyError, zipfile.BadZipFile, struct.error, IndexError) as e:
            print(f"    NOT FOR {abi}: {e}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: bitgapps-check.py ABI FOLDER")
    sys.exit(main(sys.argv[1], Path(sys.argv[2])))
