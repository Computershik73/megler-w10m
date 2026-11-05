"""A library built for another ABI, held against the armeabi-v7a library of the same name.

  abidiff.py CXXFILT ABI LIBRARY ARMEABI-V7A-LIBRARY built|copied

tools/android9-natives.sh asks this of every library it builds or copies for
arm64-v8a, x86_64 or x86. The two must need the same libraries in the same
order (DT_NEEDED), and import the same names but for what the ABI accounts for:
the names in BECAUSE below, and C++ names whose integer types the two ABIs
spell differently (size_t is `unsigned int` on a 32-bit ABI and `unsigned long`
on a 64-bit one, int64_t `long long` and `long`). Those are said, each with its
reason, `-` before a name armeabi-v7a's library imports and this one does not,
`+` before the other way round. Anything else is a reason to stop (exit status
1) for a library built here. Android's own libcrypto.so and libssl.so differ
from their ARM build in more (their ARM unwinder, their CPU probing): there it
is said and not held against them.

CXXFILT is a c++filt (the NDK's arm-linux-androideabi-c++filt: the C++ ABI's
mangling is the same for every processor).
"""
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf import Elf  # noqa: E402

# (a whole name, as a regular expression; which library has it: 'arm', armeabi-v7a's, 'abi', the other one, or
# 'both', either; the reason, as said)
BECAUSE = [
    # The ARM EABI's memcpy, memmove, memset and memclr, which ARM code calls in their place.
    (r'__aeabi_mem(cpy|move|set|clr)[48]?', 'arm', "the ARM EABI's memory functions"),
    (r'memcpy|memmove|memset', 'abi', 'for the __aeabi_ ones'),
    # ARMv7 code reads the stack guard from __stack_chk_guard; arm64 and x86_64 code, and x86 code built for API 17
    # and up, from bionic's TLS slot 5 through the thread pointer. (The NDK's x86 C++ runtime, built for API 16,
    # still reads __stack_chk_guard, so on x86 both are there.)
    (r'__stack_chk_guard', 'arm', 'the stack guard is in TLS here'),
    # Rounding is one instruction on ARMv8 (frintp, frintm, ...) and with SSE4.1 (roundsd), a call on ARMv7.
    (r'ceil|floor|rint|round|trunc|nearbyint', 'arm', 'one instruction here'),
    # libgcc's __aeabi_idiv0 and __aeabi_ldiv0 (_dvmd_lnx.o): on ARM a division by zero raises SIGFPE in software.
    (r'raise', 'arm', "ARM's __aeabi_idiv0"),
    # The NDK's ARM unwinder (libunwind.a) finds a library's .ARM.exidx with __gnu_Unwind_Find_exidx and writes its
    # messages with these.
    (r'__gnu_Unwind_Find_exidx|dladdr|fprintf|fflush', 'arm', "the NDK's ARM unwinder"),
    # libgcc's DWARF unwinder, the other ABIs': it walks the loaded libraries for their .eh_frame_hdr, and a weak
    # pthread_create tells it whether there are threads at all.
    (r'dl_iterate_phdr|pthread_create', 'abi', "libgcc's DWARF unwinder"),
    # The NDK's C++ runtime (libc++_static.a, libc++abi.a) is built for each ABI's lowest API level: 16 for the 32-bit
    # ones, where it aborts through __assert2 and calls snprintf as it is (ARM's unwinder calls snprintf too), 21 for
    # the 64-bit ones, where it says why it aborts with android_set_abort_message and syslog, has MB_CUR_MAX and
    # strtof, and is fortified.
    (r'__assert2|snprintf', 'arm', "the NDK's C++ runtime at API 16"),
    (r'android_set_abort_message|openlog|syslog|closelog|__ctype_get_mb_cur_max|strtof', 'abi',
     "the NDK's C++ runtime at API 21"),
    # 32-bit bionic's headers rename stat64, fstat64 and lstat64 to stat, fstat and lstat (struct stat is
    # struct stat64 there); 64-bit bionic has them under their own names.
    (r'stat|fstat|lstat', 'arm', "32-bit bionic's names for stat64 and the like"),
    (r'stat64|fstat64|lstat64', 'abi', "64-bit bionic's names"),
    # ojluni's UnixNativeDispatcher.c, on i386 and x86_64 only: when dlsym(RTLD_DEFAULT, "fstatat64") finds nothing,
    # it makes the system call itself, syscall(300) or syscall(262).
    (r'syscall', 'abi', "ojluni's own fstatat64 on x86"),
    # FORTIFY: a bounds check kept in one build and folded into the plain call in the other. The NDK's 64-bit C++
    # runtime is fortified, and Thumb code is built -Os, the other ABIs' -O2, which inline differently.
    (r'__\w+_chk|vsnprintf', 'both', 'FORTIFY checks kept or folded'),
]
DATA_MODEL = 'size_t or int64_t, spelled for 64 bits'       # C++ names that are one function (see portable())

_INTS = [
    (re.compile(r'\bunsigned long long\b'), 'UINT'), (re.compile(r'\bunsigned long\b'), 'UINT'),
    (re.compile(r'\bunsigned int\b'), 'UINT'),
    (re.compile(r'\blong long\b'), 'INT'), (re.compile(r'\blong\b'), 'INT'), (re.compile(r'\bint\b'), 'INT'),
]


def windows_path(p):
    if os.name == 'nt' and p.startswith('/') and len(p) > 2 and p[2] == '/':     # /c/Users/... as Git Bash has it
        return p[1].upper() + ':' + p[2:]
    return p


def demangle(cxxfilt, names):
    names = list(names)
    if not names:
        return {}
    r = subprocess.run([windows_path(cxxfilt)], input='\n'.join(names) + '\n', capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('abidiff.py: %s failed: %s' % (cxxfilt, r.stderr[:300]))
    return dict(zip(names, r.stdout.split('\n')))


def portable(demangled):
    """A demangled name with every plain integer type made INT or UINT: what is left is the same on every ABI."""
    for rx, to in _INTS:
        demangled = rx.sub(to, demangled)
    return demangled


def main():
    if len(sys.argv) != 6 or sys.argv[5] not in ('built', 'copied'):
        raise SystemExit(__doc__)
    cxxfilt, abi, path, ref_path, how = sys.argv[1:]
    mine, arm = Elf(path), Elf(ref_path)
    if mine.needed() != arm.needed():
        print('      DT_NEEDED: %s; armeabi-v7a\'s: %s' % (' '.join(mine.needed()), ' '.join(arm.needed())))
        return 1
    have, had = set(mine.imports()), set(arm.imports())
    gone, new = sorted(had - have), sorted(have - had)
    print('      the same DT_NEEDED as armeabi-v7a\'s; of %d imported names (armeabi-v7a\'s %d), %d differ'
          % (len(have), len(had), len(gone) + len(new)))

    said = collections.OrderedDict()          # reason -> ['-name', '+name', ...]
    left_gone, left_new = [], []
    for side, names, left, sign in (('arm', gone, left_gone, '-'), ('abi', new, left_new, '+')):
        for n in names:
            for rx, where, why in BECAUSE:
                if where in (side, 'both') and re.fullmatch(rx, n):
                    said.setdefault(why, []).append(sign + n)
                    break
            else:
                left.append(n)

    # C++ names that are one function, its integer types spelled as each ABI spells them.
    cxx = [n for n in left_gone + left_new if n.startswith('_Z')]
    dem = demangle(cxxfilt, cxx)
    keys_gone = collections.Counter(portable(dem[n]) for n in left_gone if n in dem)
    keys_new = collections.Counter(portable(dem[n]) for n in left_new if n in dem)
    both = keys_gone & keys_new
    for left, sign in ((left_gone, '-'), (left_new, '+')):
        pool = collections.Counter(both)
        for n in list(left):
            k = portable(dem[n]) if n in dem else None
            if k is not None and pool[k] > 0:
                pool[k] -= 1
                left.remove(n)
                said.setdefault(DATA_MODEL, []).append(sign + n)

    for why, names in said.items():
        print('        %s (%s)' % (' '.join(names), why))
    odd = ['-' + n for n in left_gone] + ['+' + n for n in left_new]
    if odd:
        print('        %s (%s)' % (' '.join(odd), 'NOT accounted for by the ABI' if how == 'built'
                                   else 'where Android\'s own ARM build differs: its unwinder, its CPU probing'))
        return 1 if how == 'built' else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
