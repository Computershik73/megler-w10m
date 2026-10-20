#!/usr/bin/env python3
"""
Writes core/src/drivers/mg_java_gles.c: the native methods of android.opengl.GLES10, GLES10Ext, GLES11, GLES11Ext,
GLES20, GLES30, GLES31 and GLES32 as the framework declares them, each a call of the OpenGL ES function of the same name.

    python tools/gles-natives.py [--check]

Two lists are matched. The Java declarations are the framework's own (core/framework/src/android/opengl/GLES*.java,
or the overlay's copy where there is one). The C prototypes are Khronos' headers as the NDK ships
them (MEGLER_NDK: sysroot/usr/include/GLES, GLES2, GLES3), which say what each parameter is in C: a value, or memory.
A Java method's parameters are then laid onto the C function's in order, the way Android's own generator does:

    a value               int, float, boolean, byte, short, long: passed as it is (a float as a float: the caller is the
                          core, not guest code, so ANGLE's own function is called and no adapter is needed)
    an array and its      int[] p, int pOffset for one pointer: the array's own memory from that element on (arrays are
    offset                never moved or copied here); a null array or an offset outside it is IllegalArgumentException
    a java.nio buffer     for one pointer: what Android's NIOAccess gives, a direct buffer's address plus its position in
                          elements, or a buffer over an array that array's memory; null is NULL where C only reads the
                          memory, IllegalArgumentException where it writes it
    an int or a long      for a pointer: an offset into the bound buffer object (glDrawElements(..., int offset)), passed
                          as the pointer it is in C
    a String              for a const GLchar *: its UTF-8

Everything that does not fit those -- what returns a String or a Buffer, takes a String[] or a callback, or differs from
C in some other way -- is not written here: core/src/drivers/mg_java_gl.c has those by hand, and a declaration neither
file has falls back to the class's "not there yet" (mg_java.c). What is written, what was left, and why, is printed.

The file also lists every declaration (k_declared), for the test that counts what is bound.

--check writes nothing and fails if the file would differ from what is there.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'core', 'src', 'drivers', 'mg_java_gles.c')
CLASSES = ['GLES10', 'GLES10Ext', 'GLES11', 'GLES11Ext', 'GLES20', 'GLES30', 'GLES31', 'GLES32']
PREFIX = {'GLES10': 'g10', 'GLES10Ext': 'g10x', 'GLES11': 'g11', 'GLES11Ext': 'g11x', 'GLES20': 'g20', 'GLES30': 'g30', 'GLES31': 'g31',
          'GLES32': 'g32'}


def msys_path(p):
    # "/c/Users/..." as Git Bash spells it, for a Windows Python.
    m = re.match(r'^/([a-zA-Z])/(.*)$', p or '')
    return '%s:/%s' % (m.group(1), m.group(2)) if m and os.name == 'nt' else p


def framework_dir():
    return os.path.join(ROOT, 'core', 'framework')


def ndk_include():
    ndk = msys_path(os.environ.get('MEGLER_NDK', ''))
    if not ndk:
        ndk = os.path.join(os.path.expanduser('~'), 'Documents', 'android-ndk-r21e')
    inc = os.path.join(ndk, 'sysroot', 'usr', 'include')
    if not os.path.isfile(os.path.join(inc, 'GLES2', 'gl2.h')):
        sys.exit('no %s/GLES2/gl2.h: Khronos\' headers come from the NDK (MEGLER_NDK)' % inc)
    return inc


# ------------------------------------------------------------------ the Java declarations

JAVA_TYPES = {
    'void': 'V', 'int': 'I', 'float': 'F', 'boolean': 'Z', 'byte': 'B', 'short': 'S', 'long': 'J', 'char': 'C', 'double': 'D',
    'String': 'Ljava/lang/String;', 'Buffer': 'Ljava/nio/Buffer;', 'ByteBuffer': 'Ljava/nio/ByteBuffer;', 'ShortBuffer': 'Ljava/nio/ShortBuffer;',
    'IntBuffer': 'Ljava/nio/IntBuffer;', 'LongBuffer': 'Ljava/nio/LongBuffer;', 'FloatBuffer': 'Ljava/nio/FloatBuffer;',
    'DebugProc': 'Landroid/opengl/GLES32$DebugProc;',
}
ELEMENT = {'int': 4, 'float': 4, 'long': 8, 'short': 2, 'byte': 1, 'boolean': 1}


def jni_type(t):
    t = t.strip()
    dims = 0
    while t.endswith('[]'):
        dims += 1
        t = t[:-2].strip()
    base = t.split('.')[-1]
    if base not in JAVA_TYPES:
        sys.exit('a Java type this does not know: %s' % t)
    return '[' * dims + JAVA_TYPES[base], base, dims


def java_natives(fw):
    decl = re.compile(r'public\s+static\s+native\s+([\w.\[\]]+)\s+(\w+)\s*\(([^)]*)\)\s*;', re.S)
    out = []
    for cls in CLASSES:
        path = os.path.join(ROOT, 'core', 'framework-overlay', 'android', 'opengl', cls + '.java')
        if not os.path.isfile(path):
            path = os.path.join(fw, 'src', 'android', 'opengl', cls + '.java')
        text = io.open(path, encoding='utf-8').read()
        for m in decl.finditer(text):
            ret, name, params = m.group(1), m.group(2), m.group(3).strip()
            plist = []
            if params:
                for p in params.split(','):
                    words = p.split()
                    plist.append((' '.join(words[:-1]), words[-1]))
            sig = '(' + ''.join(jni_type(t)[0] for t, _ in plist) + ')' + jni_type(ret)[0]
            out.append({'cls': cls, 'name': name, 'ret': ret, 'params': plist, 'sig': sig})
    return out


# ------------------------------------------------------------------ the C prototypes

def c_prototypes(inc):
    # (The result's '*' can sit against GL_APIENTRY: "const GLubyte *GL_APIENTRY glGetString".)
    proto = re.compile(r'GL_API(?:CALL)?\s+([^;(]+?)\s*GL_APIENTRY\s+(\w+)\s*\(([^)]*)\)\s*;', re.S)
    found = {}
    for sub, names in (('GLES3', ['gl32.h', 'gl31.h', 'gl3.h']), ('GLES2', ['gl2.h', 'gl2ext.h']), ('GLES', ['gl.h', 'glext.h'])):
        for name in names:
            path = os.path.join(inc, sub, name)
            if not os.path.isfile(path):
                continue
            text = io.open(path, encoding='utf-8', errors='replace').read()
            for m in proto.finditer(text):
                ret, fname, params = ' '.join(m.group(1).split()), m.group(2), ' '.join(m.group(3).split())
                if fname in found:
                    continue
                plist = []
                if params and params != 'void':
                    for p in params.split(','):
                        p = p.strip()
                        mm = re.match(r'^(.*?[\s*])(\w+)$', p)
                        # (A parameter without a name is its type alone.)
                        ctype, cname = (mm.group(1), mm.group(2)) if mm and mm.group(1).strip() else (p, '')
                        plist.append((' '.join(ctype.replace('*', ' * ').split()), cname))
                found[fname] = (ret, plist)
    return found


# The C types, as the core's own fixed-width ones.
SCALAR = {
    'GLenum': 'u32', 'GLuint': 'u32', 'GLbitfield': 'u32', 'GLint': 'i32', 'GLsizei': 'i32', 'GLfixed': 'i32', 'GLclampx': 'i32',
    'GLshort': 'i16', 'GLushort': 'u16', 'GLbyte': 'i8', 'GLubyte': 'u8', 'GLboolean': 'u8', 'GLchar': 'char',
    'GLfloat': 'float', 'GLclampf': 'float', 'GLintptr': 'intptr_t', 'GLsizeiptr': 'intptr_t', 'GLint64': 'i64', 'GLuint64': 'u64',
    'GLsync': 'void *', 'GLeglImageOES': 'void *', 'GLDEBUGPROC': 'void *', 'GLDEBUGPROCKHR': 'void *', 'void': 'void',
}


def c_kind(ctype):
    """'ptr' for memory, 'val' for a value, and the C spelling the core uses."""
    words = ctype.replace('const', ' ').split()
    if '*' in words:
        return 'ptr', 'const void *' if ctype.startswith('const') else 'void *'
    base = words[0] if words else ''
    if base not in SCALAR:
        return None, None
    spelled = SCALAR[base]
    if spelled == 'void *':
        return 'handle', spelled
    return 'val', spelled


VALUE_MEMBER = {'int': 'i', 'float': 'f', 'boolean': 'z', 'byte': 'b', 'short': 's', 'long': 'j'}


def match(java, cproto):
    """The C arguments for a Java declaration, and what has to be set up first; or a reason it does not fit."""
    cret, cparams = cproto
    jp = java['params']
    args, setup, k = [], [], 0
    for ctype, cname in cparams:
        kind, spelled = c_kind(ctype)
        if kind is None:
            return None, 'C parameter %s %s' % (ctype, cname)
        if k >= len(jp):
            return None, 'fewer Java parameters than C ones'
        jtype = jp[k][0]
        sig, base, dims = jni_type(jtype)
        if kind in ('ptr', 'handle'):
            if dims == 1 and base in ELEMENT:
                if k + 1 >= len(jp) or jp[k + 1][0] != 'int':
                    return None, 'an array with no offset after it'
                setup.append(('array', k, ELEMENT[base]))
                args.append('p%d' % k)
                k += 2
                continue
            if dims == 0 and base.endswith('Buffer'):
                # Memory C only reads may be no memory (glBufferData with no data, glTexImage2D with no pixels); memory C
                # writes may not be, which Android says with IllegalArgumentException.
                setup.append(('buffer', k, 'false' if ctype.startswith('const') or kind == 'handle' else 'true'))
                args.append('p%d' % k)
                k += 1
                continue
            if dims == 0 and base == 'int':
                args.append('(void *)(intptr_t)I(%d)' % k)
                k += 1
                continue
            if dims == 0 and base == 'long':
                args.append('(void *)(intptr_t)J(%d)' % k)
                k += 1
                continue
            if dims == 0 and base == 'String' and 'GLchar' in ctype and ctype.count('*') == 1:
                setup.append(('string', k))
                args.append('s%d' % k)
                k += 1
                continue
            return None, 'Java %s for C %s' % (jtype, ctype)
        # a value
        if dims or base not in VALUE_MEMBER:
            return None, 'Java %s for C %s' % (jtype, ctype)
        member = VALUE_MEMBER[base]
        if spelled == 'intptr_t':
            args.append('(intptr_t)%s(%d)' % (member.upper(), k))
        else:
            args.append('%s(%d)' % (member.upper(), k))
        k += 1
    if k != len(jp):
        return None, 'more Java parameters than C ones'
    # The result.
    rkind, rspelled = c_kind(cret) if cret != 'void' else ('void', 'void')
    jret = java['ret'].split('.')[-1]
    if rkind == 'void':
        if jret != 'void':
            return None, 'Java returns %s, C nothing' % jret
        store = '%s;'
    elif rkind == 'val' and jret == 'int':
        store = 'c->ret.i = (i32)%s;'
    elif rkind == 'val' and jret == 'boolean':
        store = 'c->ret.z = %s != 0;'
    elif rkind == 'handle' and jret == 'long':
        store = 'c->ret.j = (i64)(intptr_t)%s;'
    else:
        return None, 'Java returns %s, C %s' % (jret, cret)
    types = []
    for ctype, _ in cparams:
        types.append(c_kind(ctype)[1])
    return {'args': args, 'setup': setup, 'types': types, 'ret': rspelled, 'store': store}, None


# ------------------------------------------------------------------ the file

HEAD = '''/*
 * android.opengl.GLES10, GLES10Ext, GLES11, GLES11Ext, GLES20, GLES30, GLES31 and GLES32: their native methods, each a
 * call of the OpenGL ES function of the same name. GENERATED by tools/gles-natives.py from the framework's declarations
 * and Khronos' prototypes (the NDK's headers) -- do not edit; run it again.
 *
 * The function is the one the core gives native code too (mg_java_gl_fn: ANGLE's own, through the core's texture
 * policy), so there is one OpenGL ES in the process. What a Java parameter is in C is the tool's business; the helpers
 * (mg_java_gl.h) turn arrays, buffers and strings into memory and throw what Android throws. The methods that are
 * not here are mg_java_gl.c's, by hand.
 */
#include "mg_java_gl.h"

#define I(k) c->args[k].i
#define F(k) c->args[k].f
#define Z(k) c->args[k].z
#define B(k) c->args[k].b
#define S(k) c->args[k].s
#define J(k) c->args[k].j
'''


def emit(decls, protos):
    names = []
    index = {}
    bodies = []
    table = []
    left = []
    counter = {}
    for d in decls:
        if d['name'] == '_nativeClassInit':
            left.append((d, 'by hand: nothing to do'))
            continue
        cname, jdecl = d['name'], d
        if cname not in protos and cname.endswith('Bounds') and cname[:-6] in protos and d['params'] and d['params'][-1][0] == 'int':
            # glColorPointerBounds(size, type, stride, pointer, remaining): what the public glColorPointer of Android's Java calls,
            # the function itself with how much of the buffer is left after it, which C does not take.
            cname = cname[:-6]
            jdecl = dict(d, params=d['params'][:-1])
        cp = protos.get(cname)
        if not cp:
            left.append((d, 'no C prototype of that name'))
            continue
        m, why = match(jdecl, cp)
        if not m:
            left.append((d, why))
            continue
        if cname not in index:
            index[cname] = len(names)
            names.append(cname)
        n = counter.get((d['cls'], d['name']), 0)
        counter[(d['cls'], d['name'])] = n + 1
        fname = '%s_%s_%d' % (PREFIX[d['cls']], d['name'], n)
        ftype = '%s (*)(%s)' % (m['ret'], ', '.join(m['types']) if m['types'] else 'void')
        lines = ['/* %s.%s(%s) */' % (d['cls'], d['name'], ', '.join(t for t, _ in d['params'])),
                 'static void %s(mg_jcall *c)' % fname, '{']
        lines.append('    %s = (%s)mg_java_gl_fn(&g_fn[%d], "%s");' % (ftype.replace('(*)', '(*f)'), ftype, index[cname], cname))
        conds = ['f']
        for s in m['setup']:
            if s[0] == 'array':
                lines.append('    void *p%d;' % s[1])
                conds.append('mg_java_gl_array(c, %d, %d, &p%d)' % (s[1], s[2], s[1]))
            elif s[0] == 'buffer':
                lines.append('    void *p%d;' % s[1])
                conds.append('mg_java_gl_buffer(c, %d, %s, &p%d)' % (s[1], s[2], s[1]))
            elif s[0] == 'string':
                lines.append('    const char *s%d;' % s[1])
                conds.append('mg_java_gl_string(c, %d, &s%d)' % (s[1], s[1]))
        lines.append('    if (!(%s)) return;' % ' && '.join(conds) if len(conds) > 1 else '    if (!f) return;')
        call = 'f(%s)' % ', '.join(m['args'])
        lines.append('    ' + m['store'] % call)
        lines.append('}')
        bodies.append('\n'.join(lines))
        table.append('    { "android/opengl/%s", "%s", "%s", %s },' % (d['cls'], d['name'], d['sig'], fname))
    text = HEAD
    text += '\n/* What each function name was found to be, once asked for: ANGLE\'s, or "not there" (mg_java_gl_fn). */\n'
    text += 'static void *g_fn[%d];\n\n' % len(names)
    text += '\n\n'.join(bodies) + '\n\n'
    text += 'static const mg_jhle k_natives[] = {\n' + '\n'.join(table) + '\n    { NULL, NULL, NULL, NULL },\n};\n\n'
    text += '/* Every native method the framework declares in those classes, whether this file binds it or not: for the test that counts. */\n'
    text += 'static const mg_jhle k_declared[] = {\n'
    for d in decls:
        text += '    { "android/opengl/%s", "%s", "%s", NULL },\n' % (d['cls'], d['name'], d['sig'])
    text += '    { NULL, NULL, NULL, NULL },\n};\n\n'
    text += 'const mg_jhle *mg_java_gles_natives(void) { return k_natives; }\n'
    text += 'const mg_jhle *mg_java_gles_declared(void) { return k_declared; }\n'
    text += '/* The functions are looked up again (a test that gives the core another OpenGL ES). */\n'
    text += 'void mg_java_gles_forget(void) { memset(g_fn, 0, sizeof(g_fn)); }\n'
    return text, len(table), left


def main():
    check = '--check' in sys.argv[1:]
    decls = java_natives(framework_dir())
    protos = c_prototypes(ndk_include())
    text, written, left = emit(decls, protos)
    print('%d native methods declared in %s; %d written here, %d left:' % (len(decls), ', '.join(CLASSES), written, len(left)))
    for d, why in left:
        print('  %s.%s%s: %s' % (d['cls'], d['name'], d['sig'], why))
    old = io.open(OUT, encoding='utf-8').read() if os.path.isfile(OUT) else None
    if check:
        if old != text:
            sys.exit('%s is not what tools/gles-natives.py writes' % OUT)
        return
    if old != text:
        io.open(OUT, 'w', encoding='utf-8', newline='\n').write(text)
        print('wrote ' + os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    main()
