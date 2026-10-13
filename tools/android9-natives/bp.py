"""Reads one property of one module out of an AOSP Android.bp file.

  bp.py FILE MODULE PROPERTY        one value per line

tools/android9-natives.sh takes its source lists and compiler flags from
AOSP's own build rules with this, so that what is compiled is what Soong
would compile and not what a glob happens to find.

What it understands of Blueprint: modules (`type { ... }`), strings, lists,
maps, booleans, integers, `+` between lists or strings, comments, and
variable assignments at the top level. A module's value is its own plus,
before it, that of every module it names in `defaults` (looked up in the same
file), plus what Soong would add for a 32-bit ARM Android device: `target.android`,
`target.linux` (which in Android 9 means every Linux kernel, Android included),
`target.bionic`, `target.not_windows`, `target.android_arm`, `arch.arm` and
`multilib.lib32`. A `filegroup` named as ":name" in a source list is not
followed here; the script asks for it by name.
"""
import sys

DEVICE_VARIANTS = (('target', 'android'), ('target', 'linux'), ('target', 'bionic'), ('target', 'not_windows'),
                   ('target', 'android_arm'), ('arch', 'arm'), ('multilib', 'lib32'))


class Parser:
    def __init__(self, text):
        self.s = text
        self.i = 0
        self.vars = {}
        self.modules = []

    def error(self, what):
        line = self.s.count('\n', 0, self.i) + 1
        raise SystemExit('bp.py: line %d: %s' % (line, what))

    def skip(self):
        s = self.s
        while self.i < len(s):
            c = s[self.i]
            if c in ' \t\r\n':
                self.i += 1
            elif s.startswith('//', self.i):
                end = s.find('\n', self.i)
                self.i = len(s) if end < 0 else end
            elif s.startswith('/*', self.i):
                end = s.find('*/', self.i)
                if end < 0:
                    self.error('comment without an end')
                self.i = end + 2
            else:
                break

    def peek(self):
        self.skip()
        return self.s[self.i] if self.i < len(self.s) else ''

    def expect(self, c):
        if self.peek() != c:
            self.error('expected %r' % c)
        self.i += 1

    def ident(self):
        self.skip()
        start = self.i
        while self.i < len(self.s) and (self.s[self.i].isalnum() or self.s[self.i] in '_-.'):
            self.i += 1
        if start == self.i:
            self.error('expected a name')
        return self.s[start:self.i]

    def string(self):
        self.expect('"')
        out = []
        while True:
            c = self.s[self.i]
            self.i += 1
            if c == '"':
                return ''.join(out)
            if c == '\\':
                c = self.s[self.i]
                self.i += 1
                c = {'n': '\n', 't': '\t'}.get(c, c)
            out.append(c)

    def value(self):
        v = self.operand()
        while self.peek() == '+':
            self.i += 1
            w = self.operand()
            if type(v) is not type(w):
                self.error('+ between different kinds of value')
            v = v + w if not isinstance(v, dict) else dict(v, **w)
        return v

    def operand(self):
        c = self.peek()
        if c == '"':
            return self.string()
        if c == '[':
            self.i += 1
            out = []
            while self.peek() != ']':
                out.append(self.value())
                if self.peek() == ',':
                    self.i += 1
            self.i += 1
            return out
        if c == '{':
            return self.map()
        name = self.ident()
        if name in ('true', 'false'):
            return name == 'true'
        if name.lstrip('-').isdigit():
            return int(name)
        if name not in self.vars:
            self.error('unknown variable %s' % name)
        return self.vars[name]

    def map(self):
        self.expect('{')
        out = {}
        while self.peek() != '}':
            key = self.ident()
            self.expect(':')
            out[key] = self.value()
            if self.peek() == ',':
                self.i += 1
        self.i += 1
        return out

    def file(self):
        while self.peek():
            name = self.ident()
            c = self.peek()
            if c == '{':
                self.modules.append((name, self.map()))
            elif c == '=':
                self.i += 1
                self.vars[name] = self.value()
            elif self.s.startswith('+=', self.i):
                self.i += 2
                self.vars[name] = self.vars[name] + self.value()
            else:
                self.error('expected a module or an assignment after %s' % name)


def collect(modules, name, prop, seen):
    for kind, body in modules:
        if body.get('name') != name:
            continue
        if name in seen:
            raise SystemExit('bp.py: %s names itself in defaults' % name)
        out = []
        for d in body.get('defaults', []):
            out += collect(modules, d, prop, seen + [name])
        v = body.get(prop, [])
        out += v if isinstance(v, list) else [v]
        for group, variant in DEVICE_VARIANTS:
            v = body.get(group, {}).get(variant, {}).get(prop, [])
            out += v if isinstance(v, list) else [v]
        return out
    raise SystemExit('bp.py: no module named %s' % name)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    path, module, prop = sys.argv[1:]
    p = Parser(open(path, encoding='utf-8').read())
    p.file()
    for v in collect(p.modules, module, prop, []):
        print(v if not isinstance(v, bool) else ('true' if v else 'false'))


main()
