#!/bin/bash
# SQLite's source, for android.database.sqlite under a Java app, into
# build/sqlite/ (sqlite3.c and sqlite3.h: the "amalgamation", the whole
# library as one C file, which is how its authors mean it to be compiled in).
#
#   tools/sqlite-fetch.sh            (from Git Bash or WSL; needs curl, unzip, python)
#
#   https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
#   SQLite 3.54.0, 2990479 bytes
#   SHA3-256 7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
#   (the name, size and hash are those of sqlite.org's download page)
#
# SQLite is in the public domain. It is not kept in this repository: the
# file is fetched once and compiled into the core when it is there. Android 9
# itself has SQLite 3.22; a newer one reads and writes the same files.
set -euo pipefail

VERSION=3540000
YEAR=2026
SIZE=2990479
SHA3=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/sqlite"
ZIP="$OUT/sqlite-amalgamation-$VERSION.zip"
mkdir -p "$OUT"

if [ -s "$OUT/sqlite3.c" ] && [ -s "$OUT/sqlite3.h" ] && grep -q "sqlite-amalgamation-$VERSION" "$OUT/FROM" 2> /dev/null; then
  echo "have build/sqlite/sqlite3.c ($(cat "$OUT/FROM"))"
  exit 0
fi

curl -s -S -L -f -m 600 -o "$ZIP" "https://www.sqlite.org/$YEAR/sqlite-amalgamation-$VERSION.zip"
[ "$(stat -c%s "$ZIP")" = "$SIZE" ] || { echo "the file is $(stat -c%s "$ZIP") bytes, not $SIZE" >&2; exit 1; }
# (On Windows "python3" may be the Store's stand-in that only prints an advertisement: take the first one that runs.)
PYTHON=""
for candidate in python python3; do
  if "$candidate" -c "import hashlib" > /dev/null 2>&1; then PYTHON="$candidate"; break; fi
done
[ -n "$PYTHON" ] || { echo "no python to check the file's hash with" >&2; exit 1; }
got="$("$PYTHON" -c 'import hashlib, sys; print(hashlib.sha3_256(open(sys.argv[1], "rb").read()).hexdigest())' "$(cygpath -w "$ZIP" 2> /dev/null || echo "$ZIP")")"
[ "$got" = "$SHA3" ] || { echo "the file's SHA3-256 is $got, not $SHA3" >&2; exit 1; }
unzip -o -q -j "$ZIP" "sqlite-amalgamation-$VERSION/sqlite3.c" "sqlite-amalgamation-$VERSION/sqlite3.h" "sqlite-amalgamation-$VERSION/sqlite3ext.h" -d "$OUT"
echo "sqlite-amalgamation-$VERSION.zip, $SIZE bytes, SHA3-256 $SHA3" > "$OUT/FROM"
rm -f "$ZIP"
echo "build/sqlite: sqlite3.c $(stat -c%s "$OUT/sqlite3.c") bytes, sqlite3.h $(stat -c%s "$OUT/sqlite3.h") bytes ($(grep -m1 'define SQLITE_VERSION ' "$OUT/sqlite3.h" | tr -s ' '))"
