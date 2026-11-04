"""Select framework overlays in one pass (avoids thousands of grep processes on Windows)."""
import pathlib
import sys

fw, overlay, out = (pathlib.Path(p) for p in sys.argv[1:])
sources = {}
for tree in ('src', 'gen-src'):
    sources.update({p.relative_to(fw/tree).as_posix(): p for p in sorted((fw/tree).rglob('*.java'))})
for p in sorted(overlay.rglob('*.java.removed')):
    rel = p.relative_to(overlay).as_posix().removesuffix('.removed')
    if rel not in sources:
        raise SystemExit('removed class not found: ' + rel)
    del sources[rel]
sources.update({p.relative_to(overlay).as_posix(): p for p in sorted(overlay.rglob('*.java'))})
for p in sorted(overlay.rglob('*.java.members')):
    rel = p.relative_to(overlay).as_posix().removesuffix('.members')
    if rel not in sources:
        raise SystemExit('members class not found: ' + rel)
    text = sources[rel].read_text(encoding='utf-8')
    at = text.rstrip().rfind('}')
    if at < 0:
        raise SystemExit('no closing brace: ' + rel)
    dest = out/'members'/rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text[:at] + '\n' + p.read_text(encoding='utf-8') + '\n}\n', encoding='utf-8')
    sources[rel] = dest
paths = [p.as_posix() for _, p in sorted(sources.items())]
(out/'sources.txt').write_text('\n'.join(paths)+'\n', encoding='utf-8')
(out/'sources.arg').write_text('\n'.join('"'+p+'"' for p in paths)+'\n', encoding='utf-8')
(out/'stubs.arg').write_text('\n'.join('"'+p.as_posix()+'"' for p in sorted((fw/'stubs').rglob('*.java')))+'\n', encoding='utf-8')
print('framework: %d selected sources' % len(paths))
