"""Take only the three core APKs from a user-supplied BiTGApps ZIP for a local APPX."""

import hashlib
import io
import sys
import tarfile
import zipfile
from pathlib import Path


APKS = ("PrebuiltGmsCore", "GoogleServicesFramework", "Phonesky")


def main(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        for name in APKS:
            layer = f"tar/core/{name}.tar.xz"
            with tarfile.open(fileobj=io.BytesIO(bundle.read(layer)), mode="r:xz") as tar:
                member = tar.getmember(f"{name}/{name}.apk")
                with tar.extractfile(member) as source:
                    data = source.read()
            with zipfile.ZipFile(io.BytesIO(data)) as apk:
                contents = apk.namelist()
                if "AndroidManifest.xml" not in contents or "classes.dex" not in contents:
                    raise ValueError(f"{name} is not an Android APK")
            (destination / f"{name}.apk").write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            print(f"    bitgapps: {name}.apk ({len(data)} bytes, sha256 {digest})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: bitgapps-extract.py ARCHIVE.zip DESTINATION")
    main(Path(sys.argv[1]), Path(sys.argv[2]))
