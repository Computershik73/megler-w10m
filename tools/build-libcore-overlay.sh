#!/usr/bin/env bash
# Add newer APIs to Android 9 boot classes, preserving the original image.
# This DEX is loaded before core-oj.dex so a standalone compatibility class
# can also replace an older class with the same DEX descriptor.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/android9/aosp/libcore/ojluni/src/main/java"
OVERLAY="$ROOT/core/libcore-overlay"
OUT="$ROOT/build/libcore-overlay"
JDK="${MEGLER_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}"
DEX="$OUT/megler-libcore.dex"
if [ -f "$DEX" ] && [ -z "$(find "$SRC/javax/net/ssl" "$OVERLAY" "$0" -type f -newer "$DEX" -print -quit)" ]; then exit 0; fi
rm -rf "$OUT/sourcepath" "$OUT/classes"
mkdir -p "$OUT/src" "$OUT/classes" "$OUT/dex"
python - "$(cygpath -m "$SRC")" "$(cygpath -m "$OVERLAY")" "$(cygpath -m "$OUT")" <<'PY'
from pathlib import Path
import sys
src, overlay, out = map(Path, sys.argv[1:])
files = []
for members in sorted(overlay.rglob('*.java.members')):
    rel = members.relative_to(overlay).with_suffix('')
    text = (src / rel).read_text(encoding='utf-8')
    # <Class>.java.replace: blocks of <<< old === new >>>, each old text found exactly once in the AOSP source (what
    # uses an API the SDK's android.jar does not have, made of public ones).
    edits = overlay / rel.with_name(rel.name + '.replace')
    if edits.exists():
        for block in edits.read_text(encoding='utf-8').split('<<<\n')[1:]:
            body = block.split('>>>')[0]
            before, after = body.split('===\n')
            if text.count(before) != 1:
                sys.exit(f'{edits}: the text to replace is in {rel} {text.count(before)} times, not once')
            text = text.replace(before, after)
    at = text.rfind('}')
    target = out / 'src' / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text[:at] + members.read_text(encoding='utf-8') + '\n}\n', encoding='utf-8')
    files.append('"' + target.as_posix() + '"')
# A class given members whose source uses classes of its package that the SDK's android.jar does not have (Arrays:
# DualPivotQuicksort, TimSort...) names them in <Class>.java.uses: their AOSP sources go on javac's source path, and what
# it compiles of them goes into the overlay as well, the same code as the boot image's.
helpers = out / 'sourcepath'
helpers.mkdir(parents=True, exist_ok=True)
for uses in sorted(overlay.rglob('*.java.uses')):
    package = uses.relative_to(overlay).parent
    for name in uses.read_text(encoding='utf-8').split():
        target = helpers / package / (name + '.java')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text((src / package / (name + '.java')).read_text(encoding='utf-8'), encoding='utf-8')
for source in sorted(overlay.rglob('*.java')):
    files.append('"' + source.as_posix() + '"')
(out / 'sources.arg').write_text('\n'.join(files), encoding='utf-8')
PY
"$JDK/bin/javac" -source 8 -target 8 -encoding UTF-8 -cp "$(cygpath -m "$SDK/platforms/android-29/android.jar")" -sourcepath "$(cygpath -m "$OUT/sourcepath")" -implicit:class -d "$(cygpath -m "$OUT/classes")" "@$(cygpath -m "$OUT/sources.arg")"
(cd "$OUT/classes" && "$JDK/bin/jar" cf ../classes.jar .)
"$JDK/bin/java" -cp "$(cygpath -m "$SDK/build-tools/29.0.3/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(cygpath -m "$SDK/platforms/android-29/android.jar")" --output "$(cygpath -m "$OUT/dex")" "$(cygpath -m "$OUT/classes.jar")"
mv "$OUT/dex/classes.dex" "$DEX"
echo "built build/libcore-overlay/megler-libcore.dex"
