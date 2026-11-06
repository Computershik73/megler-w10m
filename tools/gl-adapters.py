#!/usr/bin/env python3
"""
Writes core/src/ndk/mg_gl_adapters.inc: an adapter for every entry point of EGL and OpenGL ES that guest code can get,
from the calling convention guest code calls with to the one ANGLE's functions have on desktop Windows (docs/DESKTOP.md).

    python tools/gl-adapters.py [--check]

Guest code calls EGL and OpenGL ES with Android's convention, which on desktop Windows is the core's own (the core is an
ELF of Android's ABI). ANGLE is Windows' code, and its functions have Windows' convention: Microsoft's x64 one on x86_64
(arguments in rcx, rdx, r8, r9 or xmm0-3 by position, 32 bytes of shadow space, rsi, rdi and xmm6-15 kept), __stdcall on
x86 (the Khronos headers' GL_APIENTRY and EGLAPIENTRY there: the function takes its arguments off the stack). So on those
two each entry point is given to guest code -- and to the core's own callers -- as an adapter: a C function of the guest's
convention that calls ANGLE's, typed with Windows' convention (MG_GLAPI, mg_base.h), and the compiler does the rest. On
ARM64 the two conventions are one for every call that is not variadic, and no EGL or OpenGL ES function is (this checks);
32-bit ARM has its own adapters, for the floats of Android's softfp (ndk/mg_egl.c, GL_FLOAT_FUNCTIONS).

The prototypes are Khronos' headers as the NDK ships them (MEGLER_NDK, r21e: sysroot/usr/include): EGL/egl.h, eglext.h,
eglext_angle.h, GLES/gl.h, glext.h, GLES2/gl2.h, gl2ext.h, GLES3/gl3.h, gl31.h, gl32.h. Each type is put as the C type
of its width and kind (GLenum u32, GLfloat float, GLint64 i64, EGLAttrib intptr_t, every handle and pointer a pointer),
which is what a calling convention looks at; a function named in several headers must have the same in each.

A function that takes a function of the caller's (a callback) cannot be passed on as it is: ANGLE would call guest code
with Windows' convention. Those are written by hand in core/src/ndk/mg_gl_adapters.c (HAND below), and so are the two
that give back what such a callback was given as (glGetPointerv's GL_DEBUG_CALLBACK_FUNCTION, eglQueryDebugKHR's
EGL_DEBUG_CALLBACK_KHR); this file declares them, their ANGLE's function, and puts them in the table. A function that
gives back a function (eglGetProcAddress, glGetVkProcAddrNV) has no adapter at all (LEFT below): eglGetProcAddress is
the core's own (mg_egl_resolve). A callback anywhere else, or a variadic function, stops this tool.

The table, sorted by name for a binary search, has each name, its adapter, the slot of ANGLE's function the adapter
calls, and how many bytes its arguments take on x86's stack (for the tests' stand-in of OpenGL ES, whose functions that
do nothing must take as many off the stack as an adapter puts there).

Also printed: the EGL and OpenGL ES names the core itself asks ANGLE for (mg_egl.c, mg_gltex.c, the Java natives) that no
header declares; on x86_64 and x86 those would be "no such function".

--check writes nothing and fails if the file would differ from what is there.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'core', 'src', 'ndk', 'mg_gl_adapters.inc')

HEADERS = [
    ('EGL', 'egl.h'), ('EGL', 'eglext.h'), ('EGL', 'eglext_angle.h'),
    ('GLES3', 'gl32.h'), ('GLES3', 'gl31.h'), ('GLES3', 'gl3.h'), ('GLES2', 'gl2.h'), ('GLES2', 'gl2ext.h'),
    ('GLES', 'gl.h'), ('GLES', 'glext.h'),
]

# Written by hand in mg_gl_adapters.c: they take a callback, or give back what one was given as.
HAND = {
    'glDebugMessageCallback': 'takes a GLDEBUGPROC',
    'glDebugMessageCallbackKHR': 'takes a GLDEBUGPROCKHR',
    'glGetPointerv': 'gives back the GLDEBUGPROC it was given (GL_DEBUG_CALLBACK_FUNCTION)',
    'glGetPointervKHR': 'gives back the GLDEBUGPROCKHR it was given (GL_DEBUG_CALLBACK_FUNCTION)',
    'eglDebugMessageControlKHR': 'takes an EGLDEBUGPROCKHR',
    'eglQueryDebugKHR': 'gives back the EGLDEBUGPROCKHR it was given (EGL_DEBUG_CALLBACK_KHR)',
    'eglSetBlobCacheFuncsANDROID': 'takes an EGLSetBlobFuncANDROID and an EGLGetBlobFuncANDROID',
}
# No adapter: what they give is a function of ANGLE's, of Windows' convention.
LEFT = {
    'eglGetProcAddress': 'the core answers it itself (mg_egl_resolve), with adapters',
    'glGetVkProcAddrNV': 'gives a function of the driver\'s (GL_NV_draw_vulkan_image, which ANGLE has not)',
}

# Each type by its width and kind: what a calling convention sees of it.
SCALARS = {
    'void': 'void', 'GLvoid': 'void',
    'char': 'char', 'GLchar': 'char',
    'GLenum': 'u32', 'GLuint': 'u32', 'GLbitfield': 'u32', 'EGLBoolean': 'u32', 'EGLenum': 'u32',
    'GLint': 'i32', 'GLsizei': 'i32', 'GLfixed': 'i32', 'GLclampx': 'i32', 'EGLint': 'i32', 'EGLNativeFileDescriptorKHR': 'i32',
    'GLboolean': 'u8', 'GLubyte': 'u8', 'GLbyte': 'i8', 'GLshort': 'i16', 'GLushort': 'u16', 'GLhalf': 'u16',
    'GLfloat': 'float', 'GLclampf': 'float', 'GLdouble': 'double',
    'GLintptr': 'intptr_t', 'GLsizeiptr': 'intptr_t', 'EGLAttrib': 'intptr_t', 'EGLAttribKHR': 'intptr_t', 'EGLsizeiANDROID': 'intptr_t',
    'GLint64': 'i64', 'GLint64EXT': 'i64', 'EGLnsecsANDROID': 'i64',
    'GLuint64': 'u64', 'GLuint64EXT': 'u64', 'EGLuint64KHR': 'u64', 'EGLTime': 'u64', 'EGLTimeKHR': 'u64', 'EGLTimeNV': 'u64',
    'EGLuint64NV': 'u64',
}
HANDLES = {
    'EGLDisplay', 'EGLConfig', 'EGLContext', 'EGLSurface', 'EGLClientBuffer', 'EGLImage', 'EGLImageKHR', 'EGLSync', 'EGLSyncKHR',
    'EGLSyncNV', 'EGLDeviceEXT', 'EGLOutputLayerEXT', 'EGLOutputPortEXT', 'EGLStreamKHR', 'EGLLabelKHR', 'EGLObjectKHR',
    'EGLNativeDisplayType', 'EGLNativeWindowType', 'EGLNativePixmapType', 'GLeglImageOES', 'GLeglClientBufferEXT', 'GLsync',
}
CALLBACKS = {
    'GLDEBUGPROC', 'GLDEBUGPROCKHR', 'EGLDEBUGPROCKHR', 'EGLSetBlobFuncANDROID', 'EGLGetBlobFuncANDROID', 'GLVULKANPROCNV',
    '__eglMustCastToProperFunctionPointerType',
}
WIDE = {'i64', 'u64', 'double'}
RESERVED = {
    'auto', 'break', 'case', 'char', 'const', 'continue', 'default', 'do', 'double', 'else', 'enum', 'extern', 'float', 'for',
    'goto', 'if', 'inline', 'int', 'long', 'register', 'restrict', 'return', 'short', 'signed', 'sizeof', 'static', 'struct',
    'switch', 'typedef', 'union', 'unsigned', 'void', 'volatile', 'while', 'bool', 'true', 'false', 'u8', 'u16', 'u32', 'u64',
    'i8', 'i16', 'i32', 'i64',
}


def msys_path(p):
    # "/c/Users/..." as Git Bash spells it, for a Windows Python.
    m = re.match(r'^/([a-zA-Z])/(.*)$', p or '')
    return '%s:/%s' % (m.group(1), m.group(2)) if m and os.name == 'nt' else p


def ndk_include():
    ndk = msys_path(os.environ.get('MEGLER_NDK', ''))
    if not ndk:
        ndk = os.path.join(os.path.expanduser('~'), 'Documents', 'android-ndk-r21e')
    inc = os.path.join(ndk, 'sysroot', 'usr', 'include')
    if not os.path.isfile(os.path.join(inc, 'EGL', 'egl.h')):
        sys.exit('no %s/EGL/egl.h: Khronos\' headers come from the NDK (MEGLER_NDK)' % inc)
    return inc


def c_type(tokens, where):
    """A declaration's type, as tokens (no name), as {c: its C type, callback: whether it is a function of the caller's}."""
    stars = [i for i, t in enumerate(tokens) if t == '*']
    words = [t for t in tokens if t not in ('*', 'const', 'struct')]
    if len(words) != 1:
        sys.exit('%s: a type this cannot read: %s' % (where, ' '.join(tokens)))
    base = words[0]
    if stars:
        # Whatever it points at is memory to the convention; whether that memory is const is kept, for the compiler.
        before = tokens[(stars[-2] + 1) if len(stars) > 1 else 0:stars[-1]]
        return {'c': 'const void *' if 'const' in before else 'void *', 'callback': False}
    if 'struct' in tokens:
        sys.exit('%s: a structure passed by value: %s' % (where, ' '.join(tokens)))
    if base in CALLBACKS:
        return {'c': 'void *', 'callback': True}
    if base in HANDLES:
        return {'c': 'void *', 'callback': False}
    if base in SCALARS:
        return {'c': SCALARS[base], 'callback': False}
    sys.exit('%s: a type this does not know: %s' % (where, base))


def parse_params(text, where):
    text = ' '.join(text.split())
    if text in ('', 'void'):
        return []
    if '...' in text:
        sys.exit('%s: variadic -- ARM64\'s two conventions differ for it, and x86_64\'s need more than a cast' % where)
    params = []
    for k, p in enumerate(text.split(',')):
        tokens = re.findall(r'\*|\[[^\]]*\]|\w+', p)
        # An array parameter (GLuint baseAndCount[2]) is a pointer.
        arrays = [t for t in tokens if t.startswith('[')]
        tokens = [t for t in tokens if not t.startswith('[')]
        name = None
        if len(tokens) > 1 and re.match(r'^\w+$', tokens[-1]) and tokens[-1] not in SCALARS and tokens[-1] not in HANDLES \
                and tokens[-1] not in CALLBACKS and tokens[-1] not in ('const', 'struct'):
            name = tokens.pop()
        tokens += ['*'] * len(arrays)
        t = c_type(tokens, '%s, parameter %d' % (where, k + 1))
        t['name'] = name
        params.append(t)
    return params


def prototypes(inc):
    proto = re.compile(r'\b(EGLAPI|GL_APICALL|GL_API)\s+([^;(){}#]+?)\s*\b(EGLAPIENTRY|GL_APIENTRY)\s+(\w+)\s*\(([^)]*)\)\s*;', re.S)
    found = {}
    counts = []
    for sub, name in HEADERS:
        path = os.path.join(inc, sub, name)
        text = io.open(path, encoding='utf-8', errors='replace').read()
        text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
        n = 0
        for m in proto.finditer(text):
            fname = m.group(4)
            where = '%s/%s: %s' % (sub, name, fname)
            ret = c_type(re.findall(r'\*|\w+', m.group(2)), where + ', its result')
            params = parse_params(m.group(5), where)
            n += 1
            sig = (ret['c'], tuple(p['c'] for p in params))
            if fname in found:
                if found[fname]['sig'] != sig:
                    sys.exit('%s differs from %s: %s against %s' % (where, found[fname]['where'], sig, found[fname]['sig']))
                found[fname]['headers'].append('%s/%s' % (sub, name))
                continue
            found[fname] = {'name': fname, 'ret': ret, 'params': params, 'sig': sig, 'where': where, 'headers': ['%s/%s' % (sub, name)]}
        counts.append(('%s/%s' % (sub, name), n))
    return found, counts


def arg_names(params):
    names, seen = [], set()
    for k, p in enumerate(params):
        n = p['name']
        if not n or n in RESERVED or n in seen or n.startswith('h_') or n.startswith('a_'):
            n = 'a%d' % k
        seen.add(n)
        names.append(n)
    return names


def ret_prefix(c):
    """The declarator's start for a result of this type: 'void *' is followed by the name or the parenthesis at once."""
    return c if c.endswith('*') else c + ' '


def stack_bytes(fn):
    return sum(8 if p['c'] in WIDE else 4 for p in fn['params'])


HEAD = '''/*
 * EGL's and OpenGL ES's entry points as guest code calls them, each an adapter to ANGLE's function of the same name as
 * Windows' convention has it (MG_GLAPI): x86_64 and x86 on desktop Windows (mg_gl_adapters.c, docs/DESKTOP.md).
 * GENERATED by tools/gl-adapters.py from Khronos' headers as the NDK ships them -- do not edit; run it again.
 *
 * h_<name> is ANGLE's function (a slot mg_gl_adapter fills), a_<name> the adapter. Every handle and pointer is a pointer
 * here, every other type the C type of its width: what a calling convention looks at.
 */
'''


def emit(found, counts):
    names = sorted(found)
    out = [HEAD]
    out.append('/*\n * Declared in: %s.\n' % ', '.join('%s (%d)' % c for c in counts))
    out.append(' * Written by hand in mg_gl_adapters.c:%s.\n' % ''.join('\n *   %s, which %s' % (n, HAND[n]) for n in sorted(HAND)))
    out.append(' * Left out, with no adapter (asked for, "no such function"):%s.\n */\n' % ''.join('\n *   %s: %s' % (n, LEFT[n]) for n in sorted(LEFT)))
    table = []
    most = 0
    for name in names:
        fn = found[name]
        if name in LEFT:
            continue
        callbacks = fn['ret']['callback'] or any(p['callback'] for p in fn['params'])
        if callbacks and name not in HAND:
            sys.exit('%s takes or gives a function of the caller\'s: it needs an adapter written by hand (HAND), or none (LEFT)' % fn['where'])
        ret = fn['ret']['c']
        types = ', '.join(p['c'] for p in fn['params']) or 'void'
        args = arg_names(fn['params'])
        decl = ', '.join('%s%s' % (ret_prefix(p['c']), a) for p, a in zip(fn['params'], args)) or 'void'
        out.append('\n/* %s */\n' % ', '.join(fn['headers']))
        out.append('static %s(MG_GLAPI *h_%s)(%s);\n' % (ret_prefix(ret), name, types))
        if name in HAND:
            out.append('static %sa_%s(%s);\n' % (ret_prefix(ret), name, decl))
        else:
            head = 'static %sa_%s(%s)' % (ret_prefix(ret), name, decl)
            body = '%sh_%s(%s);' % ('' if ret == 'void' else 'return ', name, ', '.join(args))
            out.append('%s { %s }\n' % (head, body) if len(head) + len(body) <= 140 else '%s\n{\n    %s\n}\n' % (head, body))
        size = stack_bytes(fn)
        most = max(most, size)
        table.append('    { "%s", (void *)a_%s, (void **)&h_%s, %d },' % (name, name, name, size))
    missing = sorted(set(HAND) - set(found))
    if missing:
        sys.exit('written by hand but in no header: %s' % ', '.join(missing))
    out.append('\n/* By name, as strcmp orders them (mg_gl_adapter\'s binary search). */\n')
    out.append('static const mg_gl_entry k_gl_entries[] = {\n%s\n};\n' % '\n'.join(table))
    out.append('/* The most bytes any of them takes on x86\'s stack. */\n#define MG_GL_MOST_ARG_BYTES %d\n' % most)
    return ''.join(out), len(table), most


def asked_by_core(found):
    """EGL and OpenGL ES names the core asks ANGLE for by a string: in calls of A(), angle(), mg_egl_host(), FN() and
    mg_java_gl_fn(), and in mg_gltex.c's resolve()."""
    files = ['core/src/ndk/mg_egl.c', 'core/src/ndk/mg_gltex.c', 'core/src/drivers/mg_java_gl.c', 'core/src/drivers/mg_java_webview.c',
             'core/src/drivers/mg_java_gles.c']
    calls = re.compile(r'\b(?:A\(\s*\w+\s*,|angle\(|mg_egl_host\(|FN\(\s*\w+\s*,\s*\w+\s*,|mg_java_gl_fn\(\s*&?[\w\[\]]+\s*,|resolve\()\s*"((?:egl|gl)[A-Z]\w*)"')
    asked = set()
    for f in files:
        text = io.open(os.path.join(ROOT, f), encoding='utf-8').read()
        asked.update(calls.findall(text))
    return sorted(n for n in asked if n not in found or n in LEFT), len(asked)


def main():
    check = '--check' in sys.argv[1:]
    found, counts = prototypes(ndk_include())
    text, n, most = emit(found, counts)
    print('%d entry points of EGL and OpenGL ES in the NDK\'s headers: %d adapters (%d written by hand), %d left out; '
          'at most %d bytes of arguments on x86\'s stack' % (len(found), n, len(HAND), len(LEFT), most))
    unknown, asked = asked_by_core(found)
    print('the core asks ANGLE for %d of them by name%s' % (asked, (': these are in no header, and would be "no such function" on '
                                                                    'x86_64 and x86: ' + ', '.join(unknown)) if unknown else ', every one declared'))
    old = io.open(OUT, encoding='utf-8').read() if os.path.isfile(OUT) else None
    if check:
        if old != text:
            sys.exit('%s is not what tools/gl-adapters.py writes' % OUT)
        return
    if old != text:
        io.open(OUT, 'w', encoding='utf-8', newline='\n').write(text)
        print('wrote ' + os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    main()
