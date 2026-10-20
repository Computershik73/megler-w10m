"""What tools/android9-natives.sh built, held against what it has to fit: Android's own
libraries, the bytecode, Meglet's linker and the layer's libc.

  report.py                              all of the below, into build/android9/lib/report/
  report.py libraries | layer | elf | jni | loadtest
  report.py stubdata REFERENCE BUILT     are ICU's stub data bytes the same in both libicuuc.so (the build asks)

libraries   size, DT_NEEDED, and the exported and imported names of each library
            beside the arm64 library of the same name from the Android 9 image
            (build/android9/system/lib64-for-reference). Names are compared
            demangled, with the differences a 32-bit build has to have taken
            out: size_t is `unsigned int` here and `unsigned long` there, and
            the NDK's libc++ lives in std::__ndk1 where the platform's lives in
            std::__1.
layer       every name the libraries import from libc, libm, libdl, liblog,
            libz and libstdc++, looked up in the layer's symbol tables
            (core/src: the mg_sym tables mg_bionic_find() answers from, read
            from the source), and what is not there, by the header that
            declares it.
elf         what each library asks of a dynamic linker, and whether
            core/src/ld/mg_ld.c and core/src/arm/mg_patch.c do it. The parts of
            mg_patch.c that look for instructions (PLT entries, thread-pointer
            reads, system calls) are repeated here on the file, so the numbers
            can be held against what the loader itself prints.
jni         the JNINativeMethod tables in libjavacore.so and libopenjdk.so
            (read out of the built libraries, with the class each is registered
            for taken from the source) and their exported Java_ functions,
            against the native methods core-oj.dex and core-libart.dex
            declare; and the native methods neither library has, by class,
            which are the runtime's own, with whether ART's source
            (art/runtime/native) registers each.
loadtest    what Meglet's linker said when tools/android9-natives/loadtest.sh had
            it load the libraries (report/loadtest.txt), against what the files
            and the layer's tables predict: the unresolved names, the PLT
            entries rewritten, the probe's answers. Skipped when loadtest.sh
            has not been run.

The layer's tables are read as they are on disk when this runs, and they
change: a name counted missing today may be there tomorrow.

Run from Git Bash or anything else with a Python 3.8; needs the NDK the build used.
"""
import collections
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf import Elf, R_ARM, PT_LOAD, PT_TLS, PT_GNU_RELRO  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
A9 = os.path.join(ROOT, 'build', 'android9')
AOSP = os.path.join(A9, 'aosp')
LIB = os.path.join(A9, 'lib')
UNSTRIPPED = os.path.join(A9, 'obj', 'unstripped')
REF = os.path.join(A9, 'system', 'lib64-for-reference')
OUT = os.path.join(LIB, 'report')
NDK = os.environ.get('MEGLET_NDK', r'C:\Users\User\Documents\android-ndk-r21e')
if NDK.startswith('/') and len(NDK) > 2 and NDK[2] == '/':       # /c/Users/... as Git Bash writes it
    NDK = NDK[1].upper() + ':' + NDK[2:]
TC = os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', os.environ.get('MEGLET_NDK_HOST', 'windows-x86_64'))
EXE = '.exe' if os.name == 'nt' else ''
SYSLIB = os.path.join(TC, 'sysroot', 'usr', 'lib', 'arm-linux-androideabi', '28')
SYSINC = os.path.join(TC, 'sysroot', 'usr', 'include')

BUILT = ['libicuuc.so', 'libicui18n.so', 'libnativehelper.so', 'libexpat.so', 'libjavacore.so', 'libopenjdk.so']
COPIED = ['libcrypto.so']
# The libraries Meglet's layer is itself (mg_bionic_is_system_lib); the NDK has a stub of each of these six.
SYSTEM = ['libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libz.so', 'libstdc++.so']
LINKER_OWN = {'__bss_start', '_edata', '_end', '__bss_start__', '__bss_end__', '__end__', '_bss_end__'}


def write(name, text):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    open(path, 'w', encoding='utf-8', newline='\n').write(text)
    return path


def tool(name, *args, stdin=None):
    r = subprocess.run([os.path.join(TC, 'bin', name + EXE)] + list(args), input=stdin, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('%s failed: %s' % (name, r.stderr[:500]))
    return r.stdout


# ------------------------------------------------------------------ names

def demangle(names):
    names = list(names)
    out = tool('arm-linux-androideabi-c++filt', stdin='\n'.join(names) + '\n').split('\n')
    return dict(zip(names, out))


_INTS = [
    (re.compile(r'\bunsigned long long\b'), 'UINT'), (re.compile(r'\bunsigned long\b'), 'UINT'), (re.compile(r'\bunsigned int\b'), 'UINT'),
    (re.compile(r'\blong long\b'), 'INT'), (re.compile(r'\blong\b'), 'INT'), (re.compile(r'\bint\b'), 'INT'),
]


def portable(demangled):
    """A demangled name with what differs between a 32-bit NDK build and a 64-bit platform build taken out.

    size_t and ptrdiff_t are int-sized here and long-sized there, int64_t is `long long` here and `long` there, so
    every plain integer type becomes INT or UINT; two overloads that differ only in that fall on one key, which is
    why keys are counted and not just collected.
    """
    s = demangled.replace('std::__ndk1::', 'std::__1::')
    for rx, to in _INTS:
        s = rx.sub(to, s)
    return s


_ANY_INT = re.compile(r'\b(UINT|INT|unsigned short|short)\b')


def looser(key):
    """portable() with signedness and `short` given up too: mode_t is `unsigned short` in 32-bit bionic and
    `unsigned int` in 64-bit, socklen_t `int` and `unsigned int`."""
    return _ANY_INT.sub('N', key)


def keyed(names, dem):
    c = collections.Counter()
    back = collections.defaultdict(list)
    for n in names:
        if n in LINKER_OWN:
            continue
        k = portable(dem.get(n, n))
        c[k] += 1
        back[k].append(n)
    return c, back


def pair_loosely(missing, extra):
    """Takes out of both counters the names that are one function with an integer type the two ABIs spell
    differently; returns them as (Android's key, this build's key)."""
    by_loose = collections.defaultdict(list)
    for k in extra.elements():
        by_loose[looser(k)].append(k)
    pairs = []
    for k in sorted(missing.elements()):
        cand = by_loose.get(looser(k))
        if cand:
            pairs.append((k, cand.pop()))
    for a, b in pairs:
        missing[a] -= 1
        extra[b] -= 1
    missing += collections.Counter()
    extra += collections.Counter()
    return pairs


# ------------------------------------------------------------------ the layer's symbol tables

_MACRO = re.compile(r'^\s*#\s*define\s+(\w+)\(([^)]*)\)\s+MG_(WRAP|DATA|DIRECT)\((.*)\)\s*$', re.M)
_STR = r'"((?:[^"\\]|\\.)*)"'


def _args(text, start):
    """The arguments of a macro call whose `(` is at text[start], split at top-level commas."""
    depth, i, out, cur, in_str = 0, start, [], [], False
    while i < len(text):
        c = text[i]
        if in_str:
            cur.append(c)
            if c == '\\':
                cur.append(text[i + 1])
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c == '(':
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ')':
            depth -= 1
            if depth == 0:
                out.append(''.join(cur).strip())
                return out
            cur.append(c)
        elif c == ',' and depth == 1:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return out


def _name_of(expr, params, args):
    """The string a macro's first argument makes: literals, `#param` and `param` joined."""
    out = []
    for tok in re.findall(r'#\s*\w+|\w+|' + _STR.replace('(', '(?:', 1), expr):
        if tok.startswith('"'):
            out.append(tok[1:-1])
        elif tok.startswith('#'):
            p = tok[1:].strip()
            out.append(args[params.index(p)] if p in params and params.index(p) < len(args) else p)
        elif tok in params and params.index(tok) < len(args):
            a = args[params.index(tok)]
            out.append(a[1:-1] if a.startswith('"') else a)
        else:
            out.append(tok)
    return ''.join(out)


def layer_symbols():
    """name -> the source file whose table has it, for every mg_sym the core registers (core/src, all of it)."""
    found = {}
    core = os.path.join(ROOT, 'core', 'src')
    for dirpath, _, files in os.walk(core):
        for f in sorted(files):
            if not f.endswith('.c'):
                continue
            path = os.path.join(dirpath, f)
            text = open(path, encoding='utf-8', errors='replace').read()
            text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            rel = os.path.relpath(path, ROOT).replace('\\', '/')
            macros = {'MG_WRAP': (['name', 'impl'], 'name'), 'MG_DATA': (['name', 'ptr'], 'name'), 'MG_DIRECT': (['fn'], '#fn')}
            for m in _MACRO.finditer(text):
                params = [p.strip() for p in m.group(2).split(',')]
                inner = _args('(' + m.group(4) + ')', 0)
                first = inner[0] if inner else ''
                macros[m.group(1)] = (params, first if m.group(3) != 'DIRECT' else '#' + first)
            body = _MACRO.sub('', text)
            for name, (params, expr) in macros.items():
                for m in re.finditer(r'\b%s\s*\(' % re.escape(name), body):
                    args = _args(body, m.end() - 1)
                    if not args or not args[0]:
                        continue
                    sym = _name_of(expr, params, args)
                    if re.fullmatch(r'[A-Za-z_$][\w$.]*', sym):
                        found.setdefault(sym, rel)
            for m in re.finditer(r'\.name\s*=\s*' + _STR, body):
                found.setdefault(m.group(1), rel)
    return found


# ------------------------------------------------------------------ who defines an import

class World:
    """The libraries of a load: the built ones, libcrypto, and the names of the system ones."""

    def __init__(self):
        self.libs = {}
        for so in BUILT + COPIED:
            p = os.path.join(LIB, so)
            if os.path.exists(p):
                self.libs[so] = Elf(p)
        self.exports = {so: set(e.exports()) for so, e in self.libs.items()}
        self.system = {}
        for so in SYSTEM:
            self.system[so] = set(Elf(os.path.join(SYSLIB, so)).exports())
        for so, listing in (('libc.so', 'libc-platform.syms'), ('libdl.so', 'libdl-platform.syms')):
            self.system[so] |= set(read_names(os.path.join(HERE, listing)))
        self.jvm = set(read_names(os.path.join(HERE, 'libopenjdkjvm.syms')))

    def scope(self, so):
        """Breadth-first over DT_NEEDED, as mg_ld.c's build_scope walks it; system libraries are not libraries there."""
        order, seen = [so], {so}
        i = 0
        while i < len(order):
            lib = self.libs.get(order[i])
            i += 1
            if not lib:
                continue
            for n in lib.needed():
                if n not in seen:
                    seen.add(n)
                    order.append(n)
        return order

    def provider(self, so, name):
        """Which library of `so`'s scope a linker would take `name` from."""
        for n in self.scope(so):
            if n in self.exports and name in self.exports[n] and n != so:
                return n
        for n in self.scope(so):
            if n in self.system and name in self.system[n]:
                return n
            if n == 'libopenjdkjvm.so' and name in self.jvm:
                return n
        for n in SYSTEM:                    # libcrypto.so is linked against the platform's libc, which has more than the NDK's stub
            if name in self.system[n]:
                return n
        return None


def read_names(path):
    out = []
    for line in open(path, encoding='utf-8'):
        line = line.split('#')[0].split()
        if line:
            out.append(line[0])
    return out


# ------------------------------------------------------------------ libraries

def libraries():
    world = World()
    names = set()
    refs = {}
    for so in BUILT:
        p = os.path.join(REF, so)
        refs[so] = Elf(p) if os.path.exists(p) else None
        for e in (world.libs[so], refs[so]):
            if e:
                names.update(e.exports())
                names.update(e.imports())
    dem = demangle(sorted(names))
    lines = ['| library | bytes | DT_NEEDED here | DT_NEEDED in Android 9 (arm64) | exports here / Android | not exported here | exported only here | imports here / Android | not imported here | imported only here |',
             '|---|---|---|---|---|---|---|---|---|---|']
    detail = []
    for so in BUILT + COPIED:
        e = world.libs[so]
        size = os.path.getsize(os.path.join(LIB, so))
        if so in COPIED or not refs.get(so):
            lines.append('| %s | %d | %s | (not compared) | %d | | | %d | | |' % (so, size, ' '.join(e.needed()), len(e.exports()), len(e.imports())))
            continue
        r = refs[so]
        ex_here, ex_back = keyed(e.exports(), dem)
        ex_ref, ex_ref_back = keyed(r.exports(), dem)
        im_here, im_back = keyed(e.imports(), dem)
        im_ref, im_ref_back = keyed(r.imports(), dem)
        missing_ex, extra_ex = ex_ref - ex_here, ex_here - ex_ref
        missing_im, extra_im = im_ref - im_here, im_here - im_ref
        same = pair_loosely(missing_ex, extra_ex) + pair_loosely(missing_im, extra_im)
        lines.append('| %s | %d | %s | %s | %d / %d | %d | %d | %d / %d | %d | %d |' % (
            so, size, ' '.join(e.needed()), ' '.join(r.needed()), sum(ex_here.values()), sum(ex_ref.values()),
            sum(missing_ex.values()), sum(extra_ex.values()), sum(im_here.values()), sum(im_ref.values()),
            sum(missing_im.values()), sum(extra_im.values())))
        detail.append('== %s' % so)
        if same:
            detail.append('-- the same name but for an integer type that is another one in a 32-bit build (mode_t, socklen_t): %d' % len(same))
            detail += ['   %s\n      here: %s' % p for p in same]
        for title, diff, back, of in (('exported by Android\'s, not by this one', missing_ex, ex_ref_back, None),
                                      ('exported by this one, not by Android\'s', extra_ex, ex_back, None),
                                      ('imported by Android\'s, not by this one', missing_im, im_ref_back, r),
                                      ('imported by this one, not by Android\'s', extra_im, im_back, so)):
            detail.append('-- %s: %d' % (title, sum(diff.values())))
            rows = []
            for k in sorted(diff):
                mangled = back[k][0]
                where = ''
                if of == so:
                    where = '  <- %s' % (world.provider(so, mangled) or '?')
                rows.append('   %s%s%s' % (k, '' if k == mangled else '   [%s]' % mangled, where))
            detail += rows
        detail.append('')
    # Where each library's imports come from: the library a linker with mg_ld.c's search order takes each name from.
    cols = BUILT + COPIED + SYSTEM + ['libopenjdkjvm.so', 'nowhere']
    used = [c for c in cols]
    table = []
    for so in BUILT + COPIED:
        c = collections.Counter(world.provider(so, n) or 'nowhere' for n in world.libs[so].imports())
        table.append((so, c))
    used = [c for c in cols if any(t[1].get(c) for t in table)]
    imp = ['| imports of | ' + ' | '.join(u[3:-3] if u.endswith('.so') else u for u in used) + ' |', '|---|' + '---|' * len(used)]
    for so, c in table:
        imp.append('| %s | ' % so + ' | '.join(str(c.get(u, '')) for u in used) + ' |')
    jvm = sorted(n for n in world.libs['libopenjdk.so'].imports() if world.provider('libopenjdk.so', n) == 'libopenjdkjvm.so')
    write('libraries.txt','\n'.join(lines) + '\n\n' + '\n'.join(imp) + '\n\nlibopenjdk.so imports from libopenjdkjvm.so (%d): %s\n' % (len(jvm), ' '.join(jvm)))
    write('libraries-differences.txt', '\n'.join(detail) + '\n')
    print('\n'.join(lines))
    print()
    print('\n'.join(imp))
    return 0


# ------------------------------------------------------------------ layer

_PREFERRED = ['stdio.h', 'stdlib.h', 'string.h', 'strings.h', 'unistd.h', 'fcntl.h', 'errno.h', 'ctype.h', 'wchar.h', 'wctype.h', 'locale.h',
              'time.h', 'signal.h', 'pthread.h', 'sched.h', 'semaphore.h', 'dirent.h', 'dlfcn.h', 'math.h', 'fenv.h', 'netdb.h', 'ifaddrs.h',
              'pwd.h', 'grp.h', 'termios.h', 'poll.h', 'mntent.h', 'langinfo.h', 'inttypes.h', 'setjmp.h', 'syslog.h', 'malloc.h',
              'zlib.h', 'link.h', 'libgen.h', 'utime.h', 'iconv.h', 'nl_types.h', 'search.h', 'stdio_ext.h', 'uchar.h', 'xlocale.h']


def header_index():
    """Every header of the NDK's sysroot that is bionic's own (not the kernel's, not libc++'s), as (relative name, text)."""
    out = []
    for dirpath, dirs, files in os.walk(SYSINC):
        rel = os.path.relpath(dirpath, SYSINC).replace('\\', '/')
        top = rel.split('/')[0]
        if top in ('c++', 'linux', 'asm-generic', 'drm', 'mtd', 'rdma', 'scsi', 'sound', 'video', 'xen', 'misc', 'EGL', 'GLES', 'GLES2', 'GLES3',
                   'KHR', 'OMXAL', 'SLES', 'vulkan', 'media', 'camera', 'aaudio', 'amidi', 'arm-linux-androideabi', 'aarch64-linux-android',
                   'i686-linux-android', 'x86_64-linux-android'):
            dirs[:] = []
            continue
        for f in files:
            if f.endswith('.h'):
                name = f if rel == '.' else rel + '/' + f
                out.append((name, open(os.path.join(dirpath, f), encoding='utf-8', errors='replace').read()))

    def rank(item):
        n = item[0]
        if n in _PREFERRED:
            return (0, _PREFERRED.index(n), n)
        # The header a program would include before the ones that only carry its inline or fortified forms.
        return (3 if n.startswith('android/') else 2 if n.startswith('bits/') else 1, n.count('/'), n)

    out.sort(key=rank)
    return out


# Names no header of the NDK declares, and what they are.
_NO_HEADER = {
    '__register_atfork': 'pthread.h: pthread_atfork(), which the NDK\'s crtbegin_so.o defines in every library as a call to this',
    'android_get_LD_LIBRARY_PATH': 'platform only (bionic\'s linker; tools/android9-natives/libdl-platform.syms)',
    'android_getaddrinfofornet': 'platform only (bionic; tools/android9-natives/libc-platform.syms)',
}


def header_of(name, headers):
    if name in _NO_HEADER:
        return '(' + _NO_HEADER[name] + ')'
    fn = re.compile(r'(?<![\w.>])%s\s*\(' % re.escape(name))
    for h, text in headers:
        if fn.search(text):
            return h
    obj = re.compile(r'(?<![\w.>])%s\b' % re.escape(name))
    for h, text in headers:
        if obj.search(text):
            return h
    return '(declared in no NDK header)'


def layer():
    world = World()
    have = layer_symbols()
    headers = header_index()
    uses = collections.defaultdict(set)         # (library it comes from, name) -> libraries that import it
    weak = set()
    unknown = collections.defaultdict(set)
    for so, e in world.libs.items():
        for s in e.dynsyms():
            if s.defined or not s.name:
                continue
            p = world.provider(so, s.name)
            if p is None:
                unknown[s.name].add(so)
            elif p in SYSTEM:
                uses[(p, s.name)].add(so)
                if s.bind == 'WEAK':
                    weak.add(s.name)
    total = len(uses)
    missing = {k: v for k, v in uses.items() if k[1] not in have}
    by_header = collections.defaultdict(list)
    for (p, name), users in missing.items():
        by_header[(p, header_of(name, headers))].append((name, users))
    out = ['The %d libraries import %d different names from %s.' % (len(world.libs), total, ', '.join(SYSTEM)),
           'The layer\'s tables (core/src, %d names) have %d of them; %d are not there:' % (len(have), total - len(missing), len(missing)), '']
    for (p, h) in sorted(by_header):
        rows = sorted(by_header[(p, h)])
        out.append('%s, <%s> (%d)' % (p, h, len(rows)) if not h.startswith('(') else '%s, %s (%d)' % (p, h, len(rows)))
        for name, users in rows:
            out.append('    %-34s %s%s' % (name, ' '.join(sorted(u[3:-3] for u in users)), '   (weak: may stay unresolved)' if name in weak else ''))
        out.append('')
    if unknown:
        out.append('Imported names no library of the load defines and no system library is known to have:')
        for name in sorted(unknown):
            out.append('    %-34s %s' % (name, ' '.join(sorted(unknown[name]))))
        out.append('')
    per = collections.Counter()
    for (p, name), users in missing.items():
        for u in users:
            per[u] += 1
    out.append('Missing names per library: ' + ', '.join('%s %d' % (so, per[so]) for so in world.libs))
    out.append('')
    # The runtime's own library, which the layer answers too.
    jvm = sorted(n for n in world.libs['libopenjdk.so'].imports() if world.provider('libopenjdk.so', n) == 'libopenjdkjvm.so')
    jvm_have = [n for n in jvm if n in have]
    out.append('libopenjdk.so imports %d names from libopenjdkjvm.so: %s' % (len(jvm), ' '.join(jvm)))
    out.append('The layer\'s tables have %d of them%s; not there: %s' % (
        len(jvm_have), ' (%s)' % ', '.join(sorted(set(have[n] for n in jvm_have))) if jvm_have else '',
        ' '.join(n for n in jvm if n not in have) or 'none'))
    elsewhere = collections.Counter(have[name] for (p, name), users in uses.items() if name in have and not have[name].startswith(('core/src/bionic/', 'core/src/ndk/', 'core/src/arm/')))
    if elsewhere:
        out.append('Of the system libraries\' names the layer has, these are in tables outside core/src/bionic, ndk and arm, which only a started Java runtime registers: ' +
                   ', '.join('%d in %s' % (c, f) for f, c in sorted(elsewhere.items())))
    text = '\n'.join(out) + '\n'
    write('layer-missing.txt', text)
    write('layer-imports.txt', ''.join('%s\t%s\t%s\t%s\n' % (p, name, 'provided by ' + have[name] if name in have else 'MISSING', ' '.join(sorted(users)))
                                       for (p, name), users in sorted(uses.items())))
    print(text)
    return 0


# ------------------------------------------------------------------ elf

# What core/src/ld/mg_ld.c does with each thing a library can ask for, as read from its source (apply_reloc,
# parse_dynamic, map_library, seal, mg_ld_init). "ignored" means the tag falls through parse_dynamic's default.
MG_RELOCS = {'R_ARM_NONE', 'R_ARM_RELATIVE', 'R_ARM_ABS32', 'R_ARM_REL32', 'R_ARM_GLOB_DAT', 'R_ARM_JUMP_SLOT', 'R_ARM_IRELATIVE',
             'R_ARM_TLS_DTPMOD32', 'R_ARM_TLS_DTPOFF32', 'R_ARM_TLS_TPOFF32'}
MG_TAGS = {
    'NEEDED': 'loaded first, breadth-first scope; the system libraries\' names are skipped',
    'SONAME': 'kept (a library is found again by it)',
    'HASH': 'read; gives the number of symbols',
    'GNU_HASH': 'read; lookups use it when it is there',
    'SYMTAB': 'read', 'STRTAB': 'read', 'STRSZ': 'read',
    'SYMENT': 'ignored (16 is assumed)',
    'REL': 'applied', 'RELSZ': 'read', 'RELENT': 'ignored (8 is assumed)', 'RELCOUNT': 'ignored (an optimisation hint)',
    'JMPREL': 'applied at load: nothing is bound lazily', 'PLTRELSZ': 'read', 'PLTREL': 'checked: must be REL',
    'PLTGOT': 'ignored (no lazy binding, so no resolver is ever entered)',
    'ANDROID_REL': 'applied (APS2 packed relocations)', 'ANDROID_RELSZ': 'read',
    'RELR': 'applied', 'RELRSZ': 'read', 'ANDROID_RELR': 'applied', 'ANDROID_RELRSZ': 'read',
    'RELRENT': 'ignored', 'ANDROID_RELRENT': 'ignored',
    'INIT': 'called before the init array', 'INIT_ARRAY': 'called in order, after the dependencies\' (mg_ld_init)', 'INIT_ARRAYSZ': 'read',
    'FINI': 'ignored: a library is never unloaded', 'FINI_ARRAY': 'ignored: a library is never unloaded', 'FINI_ARRAYSZ': 'ignored',
    'FLAGS': 'ignored (BIND_NOW is what the loader does anyway)', 'FLAGS_1': 'ignored (NOW: the same)',
    'VERSYM': 'ignored: names are looked up without versions', 'VERNEED': 'ignored', 'VERNEEDNUM': 'ignored',
    'VERDEF': 'ignored', 'VERDEFNUM': 'ignored',
    'RELA': 'REFUSED', 'RELASZ': 'REFUSED', 'TEXTREL': 'ignored (text is writable while it is relocated, so it would work)',
    'DEBUG': 'ignored', 'BIND_NOW': 'ignored', 'SYMBOLIC': 'ignored (a library\'s own definitions come first in any case)',
}
PF_X, PF_W = 1, 2


def _rd32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _rd16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def _arm_imm(w):
    v, r = w & 0xFF, ((w >> 8) & 15) * 2
    return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v


def patch_scan(e, relocs):
    """What arm/mg_patch.c would find in the library's executable segments: (PLT entries it rewrites, JUMP_SLOT slots
    no recognised entry jumps through, thread-pointer reads, svc sites). The same byte tests as plt_entry_slot,
    is_t32_tp_read / is_a32_tp_read and count_svc, on the file."""
    slots = set(off for tab, off, t, sym in relocs if tab == 'DT_JMPREL' and t == 22)
    hit = set()
    plt = tp = svc = 0
    for p in e.phdrs:
        if p['type'] != PT_LOAD or not p['flags'] & PF_X:
            continue
        seg = e.d[p['offset']:p['offset'] + p['filesz']]
        base = p['vaddr']
        off = (-base) % 4
        while off + 12 <= len(seg):
            w0, w1, w2 = _rd32(seg, off), _rd32(seg, off + 4), _rd32(seg, off + 8)
            slot = None
            if (w0 & 0xFFFFF000) == 0xE28FC000 and (w1 & 0xFFFFF000) == 0xE28CC000 and (w2 & 0xFFFFF000) == 0xE5BCF000:
                slot = (base + off + 8 + _arm_imm(w0) + _arm_imm(w1) + (w2 & 0xFFF)) & 0xFFFFFFFF
            elif off + 16 <= len(seg) and w0 == 0xE59FC004 and w1 in (0xE08CC00F, 0xE08FC00C) and w2 == 0xE59CF000:
                slot = (base + off + 12 + _rd32(seg, off + 12)) & 0xFFFFFFFF
            if slot is not None and slot in slots:
                plt += 1
                hit.add(slot)
                off += 12
            else:
                off += 4
        i = 0
        while i + 4 <= len(seg):
            t32 = _rd16(seg, i) == 0xEE1D and (_rd16(seg, i + 2) & 0x0FFF) == 0x0F70 and (_rd16(seg, i + 2) >> 12) not in (13, 15)
            a32 = not ((base + i) & 3) and (_rd32(seg, i) & 0xFFFF0FFF) == 0xEE1D0F70 and ((_rd32(seg, i) >> 12) & 15) not in (13, 15)
            if t32 or a32:
                tp += 1
            i += 2
        for i in range(4, len(seg) - 1, 2):
            if _rd16(seg, i) != 0xDF00:
                continue
            a, b = _rd16(seg, i - 2), _rd16(seg, i - 4)
            if (a & 0xFF00) == 0x2700 or (((b & 0xFBF0) == 0xF040 or (b & 0xFBF0) == 0xF240) and (a & 0x8F00) == 0x0700):
                svc += 1
        for i in range((-base) % 4, len(seg) - 3, 4):
            if _rd32(seg, i) == 0xEF000000:
                svc += 1
    return plt, len(slots - hit), tp, svc


_NEON_MNEMONIC = re.compile(
    r'^(vld[1-4]|vst[1-4]|vdup|vext|vtb[lx]|vrev(16|32|64)|vswp|vzip|vuzp|vtrn|vbsl|vbit|vbif|vand|vorr|veor|vbic|vorn|vmvn|vcnt|vcls|vclz|'
    r'vshl|vshr|vsra|vsli|vsri|vrshr|vrsra|vqshl|vqrshl|vqmov|vmovl|vmovn|vmull|vmlal|vmlsl|vpadd|vpaddl|vpadal|vaddl|vaddw|vsubl|vsubw|'
    r'vaddhn|vsubhn|vabd|vaba|vabal|vabdl|vmax|vmin|vpmax|vpmin|vceq|vcge|vcgt|vcle|vclt|vtst|vrecpe|vrsqrte|vqadd|vqsub|vqdmul|vhadd|vrhadd)'
    r'(\.|$)')
_NEON_SUFFIX = re.compile(r'\.(i|s|u|p)(8|16|32|64)$')
_WIDE_REG = re.compile(r'\b(q\d+|d(1[6-9]|2\d|3[01]))\b')
_LATER = re.compile(r'^(sdiv|udiv|aes\w+|sha1\w+|sha256\w+|crc32\w+|vrint\w+|vcvt[anpm]\w*|vmaxnm|vminnm|vsel\w+|vfma|vfms|vfnma|vfnms|'
                    r'lda|ldab|ldah|ldaex\w*|stl|stlb|stlh|stlex\w*|sevl|hlt|pmull)(\.|$)')


def instruction_scan(path):
    """(instructions, {kind: count}, functions with NEON) from GNU objdump's disassembly: instructions that plain
    ARMv7-A with VFPv3-D16 does not have. Only as good as objdump's idea of what is code: right where the file still
    has its symbol table with the mapping symbols ($a, $t, $d), a guess where it is stripped."""
    out = tool('arm-linux-androideabi-objdump', '-d', '--no-show-raw-insn', path)
    kinds = collections.Counter()
    funcs = collections.Counter()
    n = 0
    cur = '?'
    for line in out.split('\n'):
        if line.endswith('>:'):
            cur = line.split('<', 1)[1][:-2]
            continue
        parts = line.split('\t')
        if len(parts) < 2 or not parts[0].strip().endswith(':'):
            continue
        mn = parts[1].strip()
        ops = parts[2].split(';')[0] if len(parts) > 2 else ''
        if mn.startswith('.') or mn.startswith(';'):
            continue
        n += 1
        if _LATER.match(mn):
            kinds['later than ARMv7-A/VFPv3 (%s)' % mn.split('.')[0]] += 1
        elif _NEON_MNEMONIC.match(mn) or (_NEON_SUFFIX.search(mn) and not mn.startswith('vcvt')) or _WIDE_REG.search(ops):
            kinds['NEON, or a register VFPv3-D16 does not have'] += 1
            funcs[cur] += 1
    return n, kinds, funcs


def elf():
    out = []
    summary = []
    world = World()
    scan_rows = []
    for so in BUILT + COPIED:
        e = world.libs[so]
        relocs = e.dynamic_relocs()
        types = collections.Counter((tab, R_ARM.get(t, 'type %d' % t)) for tab, off, t, sym in relocs)
        tags = collections.OrderedDict()
        for t in e.dyn_tags():
            tags[t] = tags.get(t, 0) + 1
        dynsyms = e.dynsyms()
        weak_undef = sorted(s.name for s in dynsyms if not s.defined and s.bind == 'WEAK' and s.name)
        ifunc = [s.name for s in dynsyms if s.type == 'IFUNC']
        tls_syms = [s.name for s in dynsyms if s.type == 'TLS']
        thumb = sum(1 for s in dynsyms if s.defined and s.type == 'FUNC' and s.value & 1)
        arm = sum(1 for s in dynsyms if s.defined and s.type == 'FUNC' and not s.value & 1)
        plt, plt_missed, tp, svc = patch_scan(e, relocs)
        loads = [p for p in e.phdrs if p['type'] == PT_LOAD]
        attrs = tool('arm-linux-androideabi-readelf', '-A', os.path.join(LIB, so))
        attr = {k: v for k, v in re.findall(r'Tag_(\w+): (.*)', attrs.replace('\r', ''))}
        out.append('== %s' % so)
        out.append('segments: ' + ', '.join('%s%s%s %d KB' % ('R', 'W' if p['flags'] & PF_W else '-', 'X' if p['flags'] & PF_X else '-', (p['memsz'] + 1023) // 1024)
                                            for p in loads) + ('; ONE IS WRITABLE AND EXECUTABLE' if any(p['flags'] & PF_W and p['flags'] & PF_X for p in loads) else ''))
        out.append('other program headers: ' + ', '.join(sorted(set({PT_TLS: 'PT_TLS', PT_GNU_RELRO: 'PT_GNU_RELRO', 0x70000001: 'PT_ARM_EXIDX', 2: 'PT_DYNAMIC',
                                                                      4: 'PT_NOTE', 6: 'PT_PHDR', 0x6474E551: 'PT_GNU_STACK'}.get(p['type'], '0x%x' % p['type'])
                                                                     for p in e.phdrs if p['type'] != PT_LOAD))))
        out.append('relocations:')
        for (tab, name), c in sorted(types.items()):
            out.append('    %-16s %-20s %6d   %s' % (tab, name, c, 'applied by mg_ld.c' if name in MG_RELOCS else 'NOT HANDLED by mg_ld.c'))
        out.append('dynamic tags:')
        for t, c in tags.items():
            out.append('    %-16s %s%s' % (t, MG_TAGS.get(t, 'ignored (not a tag parse_dynamic knows)'), '' if c == 1 else '   (%d of them)' % c))
        init = sum(e.dyn('INIT_ARRAYSZ')) // 4
        fini = sum(e.dyn('FINI_ARRAYSZ')) // 4
        out.append('constructors: %d in DT_INIT_ARRAY%s; destructors: %d in DT_FINI_ARRAY (never run)' % (init, ', and a DT_INIT' if e.dyn('INIT') else '', fini))
        vn = e.verneed()
        out.append('symbol versions asked for: ' + ('; '.join('%s: %s' % (k, ' '.join(v)) for k, v in vn.items()) if vn else 'none') + ' (ignored by mg_ld.c)')
        out.append('weak undefined symbols (0 when nothing defines them): %s' % (' '.join(weak_undef) if weak_undef else 'none'))
        out.append('IFUNC symbols: %s; thread-local symbols: %s' % (len(ifunc) or 'none', len(tls_syms) or 'none'))
        out.append('exported functions: %d Thumb, %d ARM' % (thumb, arm))
        out.append('mg_patch.c would rewrite %d PLT entries for %d JUMP_SLOT relocations%s; %d thread-pointer reads; %d direct system calls' % (
            plt, sum(c for (tab, name), c in types.items() if name == 'R_ARM_JUMP_SLOT' and tab == 'DT_JMPREL'),
            '' if not plt_missed else ' (%d SLOTS HAVE NO ENTRY IT RECOGNISES)' % plt_missed, tp, svc))
        out.append('build attributes: CPU %s, FP %s, Advanced SIMD %s' % (attr.get('CPU_arch'), attr.get('FP_arch'), attr.get('Advanced_SIMD_arch', 'not used')))
        unstripped = os.path.join(UNSTRIPPED, so)
        n, kinds, funcs = instruction_scan(unstripped if so in BUILT else os.path.join(LIB, so))
        full = e.symtab() if so in COPIED else Elf(unstripped).symtab()
        arm_fns = sorted(set(s.name for s in full if s.type == 'FUNC' and s.defined and not s.value & 1))
        out.append('instructions (objdump, %s): %d; outside ARMv7-A with VFPv3-D16: %s' % (
            'with symbols' if so in BUILT else 'stripped file, so data in the text may be read as code', n,
            ', '.join('%d %s' % (c, k) for k, c in sorted(kinds.items())) if kinds else 'none'))
        if funcs:
            out.append('    in %d functions, most in: %s' % (len(funcs), ', '.join('%s (%d)' % kv for kv in funcs.most_common(12))))
        if so in BUILT:
            out.append('ARM-mode (not Thumb) functions inside, by the full symbol table: %d: %s' % (len(arm_fns), ' '.join(arm_fns)))
        out.append('')
        bad = [name for (tab, name), c in types.items() if name not in MG_RELOCS]
        summary.append('| %s | %s | %s | %d / %d | %d | %d | %s | %s | %s |' % (
            so, ', '.join('%s %d' % (name[6:], c) for (tab, name), c in sorted(types.items())),
            'APS2' if e.dyn('ANDROID_REL') else 'REL', plt,
            sum(c for (tab, name), c in types.items() if name == 'R_ARM_JUMP_SLOT'), tp, init, len(weak_undef) or 'none',
            'yes' if any(p['type'] == PT_TLS for p in e.phdrs) else 'no', 'NO: ' + ' '.join(bad) if bad else 'yes'))
    head = ['| library | relocations (dynamic) | table | PLT entries mg_patch.c rewrites / JUMP_SLOTs | thread-pointer reads | constructors | weak undefined | PT_TLS | all handled by mg_ld.c |',
            '|---|---|---|---|---|---|---|---|---|']
    write('elf.txt','\n'.join(head + summary) + '\n')
    write('elf-details.txt', '\n'.join(out) + '\n')
    print('\n'.join(head + summary))
    return 0


# ------------------------------------------------------------------ jni

def bp(path, module, prop):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'bp.py'), path, module, prop], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(r.stderr)
    return r.stdout.split()


def compiled_sources():
    """{library: [source paths]} for the two libraries with JNI tables, from the same Android.bp files the build reads."""
    core = os.path.join(AOSP, 'libcore')
    luni = os.path.join(core, 'luni', 'src', 'main', 'native')
    oj = os.path.join(core, 'ojluni', 'src', 'main', 'native')
    return {
        'libjavacore.so': [os.path.join(luni, f) for f in bp(os.path.join(luni, 'Android.bp'), 'luni_native_srcs', 'srcs')] +
                          [os.path.join(core, f) for f in bp(os.path.join(core, 'NativeCode.bp'), 'libjavacore', 'srcs') if not f.startswith(':')],
        'libopenjdk.so': [os.path.join(oj, f) for f in bp(os.path.join(oj, 'Android.bp'), 'libopenjdk_native_srcs', 'srcs')],
    }


_REGISTER = re.compile(r'jniRegisterNativeMethods\s*\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*,')
_ENTRY = re.compile(r'\b(?:OVERLOADED_)?(FAST_|CRITICAL_)?NATIVE_METHOD(?:_OVERLOAD|_AUTOSIG)?\s*\(|\{\s*"')
_FUNCTION = re.compile(r'\b(\w+)\s*\(\s*JNIEnv\s*\*\s*\w*\s*\)\s*\{')


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', ' ', text, flags=re.S))


def built_tables(so, sources):
    """[(class, method, signature, function symbol, source file, kind, register function)] for every JNINativeMethod
    table the library has a jniRegisterNativeMethods call for. Which table goes with which class, and which function
    makes the call, is read from the source; what is in the table is read from the library itself, where it lies as
    triples of pointers (a pointer to an exported function being a relocation against its name)."""
    e = Elf(os.path.join(UNSTRIPPED, so))
    syms = e.symtab()
    by_file = collections.defaultdict(dict)
    cur = None
    for s in syms:
        if s.type == 'FILE':
            cur = s.name
        elif s.type == 'OBJECT' and s.defined and cur:
            by_file[cur][s.name] = s
    fn_at = {}
    for s in syms:
        if s.type == 'FUNC' and s.defined:
            fn_at.setdefault(s.value, s.name)
    dyn = e.dynsyms()
    abs_at = {off: dyn[idx - 1].name for tab, off, t, idx in e.dynamic_relocs() if t == 2 and idx}
    out, problems = [], []
    for path in sources:
        text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
        base = os.path.basename(path)
        for m in _REGISTER.finditer(text):
            cls, var = m.group(1), m.group(2)
            callers = _FUNCTION.findall(text[:m.start()])
            caller = callers[-1] if callers else '?'
            # One step up when the call is in a helper (libcore_icu_ICU.cpp registers from a constructor that
            # register_libcore_icu_ICU makes): the register_... function of this file that names the helper.
            if not caller.startswith('register_'):
                for f in _FUNCTION.finditer(text):
                    nxt = _FUNCTION.search(text, f.end())
                    body = text[f.end():nxt.start() if nxt else len(text)]
                    if f.group(1).startswith('register_') and re.search(r'\b%s\b' % re.escape(caller), body):
                        caller = f.group(1)
                        break
            sym = by_file.get(base, {}).get(var) or by_file.get(base, {}).get('_ZL%d%s' % (len(var), var))
            if not sym:
                problems.append('%s: the table %s registered for %s in %s is not in the library' % (so, var, cls, base))
                continue
            data = e.read(sym.value, sym.size)
            if data is None or sym.size % 12:
                problems.append('%s: the table %s of %s is not %d-byte entries in a loaded segment' % (so, var, base, 12))
                continue
            # The macro each entry was written with, in order: FAST_ and CRITICAL_ say how the runtime may call it.
            body = re.search(r'JNINativeMethod\s+%s\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;' % re.escape(var), text, re.S)
            kinds = [(k.group(1) or 'normal_').rstrip('_').lower() for k in _ENTRY.finditer(body.group(1))] if body else []
            if len(kinds) != sym.size // 12:
                kinds = ['?'] * (sym.size // 12)
            for i in range(0, sym.size, 12):
                name_p, sig_p, fn_p = struct.unpack_from('<III', data, i)
                fn = abs_at.get(sym.value + i + 8) if not fn_p else fn_at.get(fn_p)
                if not name_p or not sig_p or (not fn_p and fn is None):
                    problems.append('%s: an entry of %s in %s is empty in the file (filled in at run time?)' % (so, var, base))
                    continue
                name = e.cstr(e.offset_of(name_p))
                sig = e.cstr(e.offset_of(sig_p))
                if fn is None:
                    problems.append('%s: %s.%s%s points at 0x%x, where no function starts' % (so, cls, name, sig, fn_p))
                kind = kinds[i // 12]
                # A C++ function's own name says what it takes: one without a JNIEnv* is a @CriticalNative one.
                if fn and fn.startswith('_Z') and 'P7_JNIEnv' not in fn:
                    kind = 'critical'
                out.append((cls, name, sig, fn or '?', base, kind, caller))
    return out, problems


def called_from_onload(sources):
    """The register_... functions a library's Register.cpp calls from JNI_OnLoad."""
    for path in sources:
        if os.path.basename(path) == 'Register.cpp':
            text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
            return set(re.findall(r'\bREGISTER\(\s*(\w+)\s*\)', text)) | set(re.findall(r'^\s*(register_\w+)\s*\(\s*env\s*\)\s*;', text, re.M))
    return set()


def dex_natives(dex):
    """{(class, method, signature)} of the native methods a DEX file declares, by tools/dexasm.py's listing."""
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'dexasm.py'), 'dump', dex], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('dexasm.py dump %s: %s' % (dex, r.stderr[:300]))
    out = set()
    all_methods = set()
    cls = None
    for line in r.stdout.split('\n'):
        w = line.split()
        if not w:
            continue
        if w[0] == '.class':
            cls = w[-1][1:-1]
        elif w[0] == '.method':
            name, sig = w[-1].split('(', 1)
            all_methods.add((cls, name, '(' + sig))
            if 'native' in w[1:-1]:
                out.add((cls, name, '(' + sig))
    return out, all_methods


def jni_mangle(s):
    out = []
    for c in s:
        if c == '/':
            out.append('_')
        elif c == '_':
            out.append('_1')
        elif c == ';':
            out.append('_2')
        elif c == '[':
            out.append('_3')
        elif c.isascii() and c.isalnum():
            out.append(c)
        else:
            out.append('_0%04x' % ord(c))
    return ''.join(out)


def art_natives():
    """{(class, method, signature)} that art/runtime/native/*.cc register, and the file of each."""
    d = os.path.join(AOSP, 'art', 'runtime', 'native')
    out = {}
    if not os.path.isdir(d):
        return out
    macro = re.compile(r'\b(?:OVERLOADED_)?(?:FAST_|CRITICAL_)?NATIVE_METHOD\s*\(\s*\w+\s*,\s*(\w+)\s*,\s*((?:"[^"]*"\s*)+)')
    for f in sorted(os.listdir(d)):
        if not f.endswith('.cc'):
            continue
        text = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
        text = re.sub(r'//[^\n]*', '', text)
        cls = re.search(r'REGISTER_NATIVE_METHODS\s*\(\s*"([^"]+)"', text)
        if not cls:
            continue
        for name, sig in macro.findall(text):
            out[(cls.group(1), name, ''.join(re.findall(r'"([^"]*)"', sig)))] = f
    return out


def jni():
    sources = compiled_sources()
    natives, declared = {}, set()
    for dex in ('core-oj.dex', 'core-libart.dex'):
        n, allm = dex_natives(os.path.join(A9, 'boot', dex))
        declared |= allm
        for k in n:
            natives[k] = dex
    classes = set(c for c, _, _ in declared)
    out, problems = [], []
    registered = {}
    never = collections.defaultdict(list)
    per_lib = collections.Counter()
    for so in ('libjavacore.so', 'libopenjdk.so'):
        rows, p = built_tables(so, sources[so])
        problems += p
        called = called_from_onload(sources[so])
        for cls, name, sig, fn, src, kind, caller in rows:
            if caller not in called:
                never[(so, src, caller, cls)].append('%s%s' % (name, sig))
                continue
            per_lib[so] += 1
            key = (cls, name, sig.lstrip('!'))
            if key in registered:
                problems.append('%s.%s%s is in two tables (%s and %s)' % (cls, name, sig, registered[key][1], src))
            registered[key] = (so, src, fn, kind)
    # Tables against the bytecode.
    wrong = []
    for (cls, name, sig), (so, src, fn, kind) in sorted(registered.items()):
        if (cls, name, sig) in natives:
            continue
        if cls not in classes:
            wrong.append('%s.%s%s (%s, %s): neither DEX file has the class' % (cls, name, sig, so, src))
        elif (cls, name, sig) in declared:
            wrong.append('%s.%s%s (%s, %s): the method is there but is not native' % (cls, name, sig, so, src))
        else:
            near = sorted(s for c, n, s in natives if c == cls and n == name)
            wrong.append('%s.%s%s (%s, %s): no such native method%s' % (cls, name, sig, so, src, '; the class declares %s%s' % (name, ' and '.join(near)) if near else ''))
    # Exported Java_ functions against the bytecode.
    exported = {}
    for so in ('libjavacore.so', 'libopenjdk.so'):
        for n in Elf(os.path.join(LIB, so)).exports():
            if n.startswith('Java_'):
                exported[n] = so
    by_export = {}
    short_count = collections.Counter('Java_%s_%s' % (jni_mangle(c), jni_mangle(n)) for c, n, s in natives)
    for (cls, name, sig) in natives:
        short = 'Java_%s_%s' % (jni_mangle(cls), jni_mangle(name))
        long = short + '__' + jni_mangle(sig[1:sig.index(')')])
        for cand in (long, short):
            if cand in exported:
                if cand == short and short_count[short] > 1:
                    problems.append('%s serves %d overloads of %s.%s by its short name' % (short, short_count[short], cls, name))
                by_export.setdefault(cand, []).append((cls, name, sig))
                break
    orphans = sorted(set(exported) - set(by_export))
    covered_by_export = set(k for v in by_export.values() for k in v)
    both = sorted(covered_by_export & set(registered))
    # What is left is the runtime's.
    art = art_natives()
    left = sorted(set(natives) - set(registered) - covered_by_export)
    by_class = collections.defaultdict(list)
    for k in left:
        by_class[k[0]].append(k)
    nowhere = [k for k in left if k not in art]
    art_and_lib = sorted(k for k in art if k in registered or k in covered_by_export)

    kinds = collections.Counter(v[3] for v in registered.values())
    out.append('core-oj.dex declares %d native methods and core-libart.dex %d: %d in all.' % (
        sum(1 for v in natives.values() if v == 'core-oj.dex'), sum(1 for v in natives.values() if v == 'core-libart.dex'), len(natives)))
    out.append('From JNI_OnLoad libjavacore.so registers %d methods in JNINativeMethod tables and libopenjdk.so %d (%d different methods; %s).' % (
        per_lib['libjavacore.so'], per_lib['libopenjdk.so'], len(registered), ', '.join('%d %s' % (c, k) for k, c in sorted(kinds.items()))))
    out.append('Of those, %d are native methods the two DEX files declare with that name and signature; %d are not:' % (len(registered) - len(wrong), len(wrong)))
    out += ['    ' + w for w in wrong] or ['    (none)']
    out.append('Tables that are in a library but whose registering function JNI_OnLoad never calls: %d' % len(never))
    for (so, src, caller, cls), methods in sorted(never.items()):
        out.append('    %s, %s: %s() would register %d methods for %s: %s' % (so, src, caller, len(methods), cls, ' '.join(methods)))
    out.append('')
    out.append('The two libraries export %d Java_ functions (libopenjdk.so %d, libjavacore.so %d), which a runtime finds by name' % (
        len(exported), sum(1 for v in exported.values() if v == 'libopenjdk.so'), sum(1 for v in exported.values() if v == 'libjavacore.so')))
    out.append('(dlsym) the first time the method is called. %d of them are the JNI name of a native method the DEX files declare' % len(by_export))
    out.append('(%d methods); %d are of no declared native method:' % (len(covered_by_export), len(orphans)))
    out += ['    ' + o for o in orphans] or ['    (none)']
    if both:
        out.append('Both in a table and exported as Java_ (the table wins once it is registered): %d' % len(both))
        out += ['    %s.%s%s' % k for k in both]
    out.append('')
    if problems:
        out.append('Things that did not read as expected (%d):' % len(problems))
        out += ['    ' + p for p in problems]
        out.append('')
    out.append('Native methods the DEX files declare and neither library has: %d, in %d classes. They are the runtime\'s own.' % (len(left), len(by_class)))
    out.append('art/runtime/native/*.cc registers %d of them; %d are in no table found anywhere%s.' % (
        len(left) - len(nowhere), len(nowhere), '' if art else ' (art/runtime/native is not on disk, so none could be looked up)'))
    out.append('')
    for cls in sorted(by_class, key=lambda c: (-len(by_class[c]), c)):
        rows = by_class[cls]
        src = sorted(set(art[k] for k in rows if k in art))
        out.append('%s (%d)%s' % (cls, len(rows), '   ART: ' + ', '.join(src) if src else '   ART: no table found'))
        for k in rows:
            out.append('    %s%s%s' % (k[1], k[2], '' if k in art else '      <- in no ART table'))
    out.append('')
    if art_and_lib:
        out.append('Registered by ART and also provided by one of the two libraries (%d); whichever registers last is called:' % len(art_and_lib))
        out += ['    %s.%s%s  (ART: %s)' % (k + (art[k],)) for k in art_and_lib]
        out.append('')
    art_only = sorted(k for k in art if k not in natives)
    out.append('In an ART table but not a native method of these two DEX files (%d; other jars\', or a signature that differs):' % len(art_only))
    out += ['    %s.%s%s  (%s)' % (k + (art[k],)) for k in art_only]
    text = '\n'.join(out) + '\n'
    write('jni.txt', text)
    rows = ['%s\t%s\t%s\t%s\t%s\t%s\t%s' % (k + v) for k, v in sorted(registered.items())]
    rows += ['%s\t%s\t%s\t%s\t(exported)\t%s\tby name' % (k + (exported[n], n)) for n, ks in sorted(by_export.items()) for k in ks]
    write('jni-provided.tsv', 'class\tmethod\tsignature\tlibrary\tsource\tfunction\tkind\n' + '\n'.join(rows) + '\n')
    summary = text.split('Native methods the DEX files declare and neither library has')[0]
    print(summary)
    print('Native methods left to the runtime: %d in %d classes; ART registers %d, %d found in no table.' % (len(left), len(by_class), len(left) - len(nowhere), len(nowhere)))
    for cls in sorted(by_class, key=lambda c: (-len(by_class[c]), c)):
        print('    %-52s %3d%s' % (cls, len(by_class[cls]), '' if all(k in art for k in by_class[cls]) else '   (%d in no ART table)' % sum(1 for k in by_class[cls] if k not in art)))
    return 0


# ------------------------------------------------------------------ loadtest

def loadtest():
    """What Meglet's linker said when it loaded the libraries (loadtest.sh's output) against what the files say."""
    path = os.path.join(OUT, 'loadtest.txt')
    if not os.path.exists(path):
        print('no %s: run tools/android9-natives/loadtest.sh first' % path)
        return 1
    text = open(path, encoding='utf-8', errors='replace').read()
    world = World()
    have = layer_symbols()
    said = collections.defaultdict(set)
    for so, name in re.findall(r'ld: (\S+): unresolved (?:function|symbol|data symbol) (\S+)', text):
        said[so].add(name)
    loaded = {so: tuple(int(x) for x in m) for so, *m in
              re.findall(r'ld: (\S+) at \S+ \(\d+ KB\), (\d+) Thumb / (\d+) ARM functions, (\d+) thread-pointer reads and (\d+) PLT entries rewritten', text)}
    out = [line for line in text.split('\n') if line.startswith('mg_tests:') or line.startswith('--- ')]
    # The program may be older than the layer's source is now: a name the source has gained since is not a disagreement.
    built = re.search(r'built at (\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d)', text)
    newest = max(os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(os.path.join(ROOT, 'core', 'src')) for f in fs)
    import datetime
    stale = bool(built) and datetime.datetime(*[int(x) for x in built.groups()]) + datetime.timedelta(minutes=1) < datetime.datetime.fromtimestamp(newest)
    if stale:
        out.append('the layer\'s source has changed since that mg_tests was built (newest file under core/src: %s)' %
                   datetime.datetime.fromtimestamp(newest).strftime('%Y-%m-%d %H:%M'))
    anywhere = set().union(*world.exports.values())
    ok = True
    since, runtime = set(), set()
    for so, e in world.libs.items():
        if so not in loaded:
            out.append('%s: NOT LOADED' % so)
            ok = False
            continue
        # mg_ld.c's resolve(): the library's scope, then the layer's tables, then any loaded library.
        expect = set(s.name for s in e.dynsyms() if not s.defined and s.name and s.bind != 'WEAK' and s.name not in anywhere and s.name not in have)
        relocs = e.dynamic_relocs()
        plt, missed, tp, svc = patch_scan(e, relocs)
        thumb = sum(1 for s in e.dynsyms() if s.defined and s.type == 'FUNC' and s.value & 1)
        arm = sum(1 for s in e.dynsyms() if s.defined and s.type == 'FUNC' and not s.value & 1)
        same_counts = loaded[so] == (thumb, arm, tp, plt)
        line = '%s: loaded; %d names unresolved' % (so, len(said[so]))
        # Tables outside bionic/, ndk/ and arm/ are registered by whoever starts a Java runtime, which --load does not.
        later = set(n for n in said[so] - expect if n in have and not have[n].startswith(('core/src/bionic/', 'core/src/ndk/', 'core/src/arm/')))
        runtime |= later
        gained = set(n for n in said[so] - expect - later if n in have) if stale else set()
        since |= gained
        if said[so] - gained - later == expect:
            line += ', the %d the file and the layer\'s tables predict' % len(expect)
            if later:
                line += ', and %d that are in a table only a Java runtime registers' % len(later)
            if gained:
                line += ' and %d the layer\'s source has since gained' % len(gained)
        else:
            ok = False
            line += '; PREDICTED %d: only the loader %s; only the prediction %s' % (len(expect), sorted(said[so] - expect), sorted(expect - said[so]))
        line += '; %d PLT entries rewritten of %d' % (loaded[so][3], sum(1 for r in relocs if r[2] == 22))
        if not same_counts:
            ok = False
            line += '; COUNTS DIFFER: the loader says %s, the file (Thumb, ARM, thread-pointer reads, PLT) %s' % (loaded[so], (thumb, arm, tp, plt))
        out.append(line)
    allnames = set().union(*[v for k, v in said.items() if k in world.libs]) if said else set()
    out.append('%d different names unresolved in all: %d of libopenjdkjvm.so, %d of the system libraries' % (
        len(allnames), len(allnames & world.jvm), len(allnames - world.jvm)))
    if runtime:
        by_file = collections.defaultdict(list)
        for n in sorted(runtime):
            by_file[have[n]].append(n)
        for f, names in sorted(by_file.items()):
            out.append('unresolved in that run because `mg_tests --load` starts no Java runtime, but in the table of %s (%d): %s' % (f, len(names), ' '.join(names)))
    if since:
        out.append('unresolved in that run, in the layer\'s source now: %s' % ' '.join(sorted(since)))
    probe = re.findall(r'^I/probe: (.*) -- (ok|WRONG)$', text, re.M)
    if probe:
        out.append('the probe library (probe.c) asked %d things of ICU, expat and fdlibm: %d right, %d wrong%s' % (
            len(probe), sum(1 for p in probe if p[1] == 'ok'), sum(1 for p in probe if p[1] != 'ok'),
            ''.join('\n    WRONG: ' + p[0] for p in probe if p[1] != 'ok')))
        ok = ok and all(p[1] == 'ok' for p in probe)
    known = {'SHA256("abc")': 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
             'AES-128 of zeros with a zero key': '66e94bd4ef8a2c3b884cfa59ca342b2e',
             'BN_mul': '121932631137021795226185032733622923332237463801111263526900'}
    for what, right in known.items():
        m = re.search(r'^  %s: (\S+)$' % re.escape(what), text, re.M)
        if m:
            out.append('libcrypto.so, mg_tests\' own %s: %s' % (what, 'right' if m.group(1) == right else 'WRONG (%s)' % m.group(1)))
            ok = ok and m.group(1) == right
    failed = re.findall(r'^.*(?:relocation failed|not supported|no room for|out of an island|did not load|cannot make).*$', text, re.M)
    out.append('other complaints of the loader: ' + ('; '.join(failed) if failed else 'none'))
    out.append('what the loader and the files say %s' % ('agree' if ok else 'DO NOT AGREE'))
    write('loadtest-summary.txt', '\n'.join(out) + '\n')
    print('\n'.join(out))
    return 0


# ------------------------------------------------------------------ stubdata

def stubdata(ref_path, built_path):
    got = []
    for path in (ref_path, built_path):
        e = Elf(path)
        sym = [s for s in e.dynsyms() if s.name == 'icudt60_dat' and s.defined]
        if not sym:
            print('%s does not define icudt60_dat' % path)
            return 1
        got.append(e.read(sym[0].value, sym[0].size))
    if got[0] != got[1]:
        print('icudt60_dat differs:\n  Android: %s\n  built:   %s' % (got[0].hex(' '), got[1].hex(' ')))
        return 1
    print('    icudt60_dat: the same %d bytes as in Android\'s libicuuc.so' % len(got[0]))
    return 0


def main():
    cmds = {'libraries': libraries, 'layer': layer, 'elf': elf, 'jni': jni, 'loadtest': loadtest}
    if len(sys.argv) == 4 and sys.argv[1] == 'stubdata':
        return stubdata(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 1:
        rc = 0
        for name in cmds:
            if name == 'loadtest' and not os.path.exists(os.path.join(OUT, 'loadtest.txt')):
                continue                    # loadtest.sh was not run; nothing to hold against the files
            print('=== %s ===' % name)
            rc |= cmds[name]()
        print('all of it: %s' % OUT)
        return rc
    if len(sys.argv) == 2 and sys.argv[1] in cmds:
        return cmds[sys.argv[1]]()
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
