"""Invalidate objects with conditional optional dependencies when build features change."""
import json
from pathlib import Path
import sys

root, obj = (Path(p).resolve() for p in sys.argv[1:3])
features = {'sqlite': sys.argv[3] == '1', 'webp': sys.argv[4] == '1'}
for name in features.copy():
    source = root/'build'/name/'FROM'
    features[name + '_source'] = source.read_text() if features[name] and source.exists() else ''
stamp = obj/'core-features.json'
previous = json.loads(stamp.read_text()) if stamp.exists() else None
if previous != features:
    removed = 0
    for base, target in ((root/'core/src', obj/'core'), (root/'core/tests', obj/'tests')):
        for source in base.rglob('*.c'):
            if base.name == 'tests' and source.parent != base:
                continue
            text = source.read_text(encoding='utf-8', errors='replace')
            if not any(m in text for m in ('MG_HAVE_SQLITE', 'MG_HAVE_WEBP')):
                continue
            compiled = (target/source.relative_to(base)).with_suffix('.obj').resolve()
            assert compiled.is_relative_to(obj) and compiled.suffix == '.obj'
            if compiled.exists():
                compiled.unlink()
                removed += 1
    obj.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps(features, sort_keys=True))
    print('optional core features changed: %d conditional objects invalidated' % removed)
