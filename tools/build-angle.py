r"""Builds Megler's own ANGLE: gecko-w10m's, with Megler's changes to it (angle/*.patch).

    python tools/build-angle.py [--check]

gecko-w10m builds ANGLE as part of Firefox (its object directory, C:\rw-obj by
default). Megler's package uses that ANGLE as it is unless build/angle/dist has
one of Megler's own, which this makes without touching gecko-w10m's sources or
its object directory:

  1. Each patch in angle/ is applied to copies of the files it changes
     (build/angle/src/<the file's path in ANGLE>).
  2. Every object of libGLESv2.dll that has one of those files in it -- a
     "unified" file of several sources, or a source of its own -- is compiled
     again from the copies, with the compiler and the flags gecko-w10m's build
     used for it (its directory's backend.mk, config/autoconf.mk).
  3. libGLESv2.dll is linked again from gecko-w10m's objects and these, with
     its static libraries, its .def and .res, mozglue and the system's
     libraries, into build/angle/dist with its map in build/angle/maps
     (tools/build-appx.sh needs the map to mend the DLL's virtual-call thunks).
     libEGL.dll and mozglue.dll are gecko-w10m's own, copied beside it.

--check: no patch applied; the objects are compiled and linked all the same,
to show that the way of building gives an ANGLE that works.

Environment: MEGLER_GECKO (gecko-w10m's checkout, ../gecko-w10m by default),
MEGLER_ANGLE_OBJ (its object directory, C:\rw-obj by default).
"""
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GECKO = os.environ.get("MEGLER_GECKO") or os.path.join(os.path.dirname(ROOT), "gecko-w10m")
OBJ = os.environ.get("MEGLER_ANGLE_OBJ") or "C:\\rw-obj"
ANGLE_SRC = os.path.join(GECKO, "engine", "firefox", "third_party", "angle")
ANGLE_OBJ = os.path.join(OBJ, "third_party", "angle")
GLES_OBJ = os.path.join(ANGLE_OBJ, "libGLESv2_gn")
OUT = os.path.join(ROOT, "build", "angle")
PATCHES = os.path.join(ROOT, "angle")


def die(message):
    print("build-angle: " + message)
    sys.exit(1)


def norm(path):
    return os.path.normcase(os.path.normpath(path))


def make_vars(path):
    """The variables a generated .mk file sets, as text (+= joined with a space)."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_.]*)\s*(\+?=|:=)\s*(.*)$", line.rstrip("\n"))
            if not m:
                continue
            name, op, value = m.groups()
            out[name] = (out.get(name, "") + " " + value).strip() if op == "+=" else value
    return out


def patched_files(patch):
    files = []
    with open(patch, encoding="utf-8") as f:
        for line in f:
            if line.startswith("+++ "):
                name = line[4:].strip().split("\t")[0]
                files.append(name[2:] if name.startswith("b/") else name)
    return files


def apply_patches(check):
    src = os.path.join(OUT, "src")
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    changed = []
    if check:
        return changed
    patches = sorted(p for p in os.listdir(PATCHES) if p.endswith(".patch")) if os.path.isdir(PATCHES) else []
    for name in patches:
        patch = os.path.join(PATCHES, name)
        for rel in patched_files(patch):
            dest = os.path.join(src, rel)
            if not os.path.exists(dest):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(os.path.join(ANGLE_SRC, rel), dest)
                changed.append(rel)
        with open(patch, "rb") as f:
            r = subprocess.run(["git", "apply", "--unidiff-zero", "--directory=", "-p1", "--unsafe-paths", "-"], cwd=src, input=f.read(),
                               capture_output=True)
        if r.returncode:
            die("%s does not apply: %s" % (name, r.stderr.decode(errors="replace").strip()))
        print("  applied %s (%s)" % (name, ", ".join(patched_files(patch))))
    return changed


def objects():
    with open(os.path.join(GLES_OBJ, "libGLESv2_dll.list"), encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip()]


def units_to_build(changed, check):
    """(object as listed, its source in the object directory or the source tree) for each object that has a changed file in it."""
    units = []
    for listed in objects():
        obj = os.path.normpath(os.path.join(GLES_OBJ, listed))
        base = os.path.splitext(os.path.basename(obj))[0]
        unified = os.path.join(os.path.dirname(obj), base + ".cpp")
        if os.path.exists(unified):
            with open(unified, encoding="utf-8") as f:
                text = f.read()
            includes = [os.path.normpath(os.path.join(os.path.dirname(unified), inc)) for inc in re.findall(r'#include "([^"]+)"', text)]
            if check and "d3d11_backend" in unified and base.endswith("gn0") or any(norm(i) == norm(os.path.join(ANGLE_SRC, c)) for i in includes for c in changed):
                units.append((listed, unified, True))
        else:
            for c in changed:
                if os.path.splitext(os.path.basename(c))[0] == base:
                    units.append((listed, os.path.join(ANGLE_SRC, c), False))
    return units


def compile_unit(autoconf, listed, source, unified, changed):
    obj_dir = os.path.dirname(os.path.normpath(os.path.join(GLES_OBJ, listed)))
    flags = make_vars(os.path.join(obj_dir, "backend.mk")).get("COMPUTED_CXXFLAGS", "")
    out_dir = os.path.join(OUT, "obj", os.path.relpath(obj_dir, ANGLE_OBJ))
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(listed))[0]
    patched = {norm(os.path.join(ANGLE_SRC, c)): os.path.join(OUT, "src", c) for c in changed}
    if unified:
        lines = []
        with open(source, encoding="utf-8") as f:
            for line in f:
                m = re.match(r'(\s*#include ")([^"]+)(".*)', line.rstrip("\n"))
                if m:
                    target = os.path.normpath(os.path.join(os.path.dirname(source), m.group(2)))
                    target = patched.get(norm(target), target)
                    line = m.group(1) + target.replace("\\", "/") + m.group(3) + "\n"
                lines.append(line)
        cpp = os.path.join(out_dir, base + ".cpp")
        with open(cpp, "w", encoding="utf-8") as f:
            f.writelines(lines)
    else:
        cpp = patched.get(norm(source), source)
    obj = os.path.join(out_dir, base + ".obj")
    env = dict(os.environ, INCLUDE=autoconf["INCLUDE"].replace("/", "\\"))
    # The include directories of the unit's own directory first, as the object directory has them.
    # (The flags are make's, quoted for a POSIX shell: split as one would.)
    cmd = shlex.split(autoconf["CXX"]) + shlex.split(flags) + ["-I" + obj_dir.replace("\\", "/"), "-c", "-Fo" + obj.replace("\\", "/"), cpp.replace("\\", "/")]
    r = subprocess.run(cmd, cwd=obj_dir, env=env, capture_output=True, text=True)
    if r.returncode:
        die("compiling %s failed:\n%s%s" % (base, r.stdout[-4000:], r.stderr[-4000:]))
    print("  compiled %s" % os.path.relpath(obj, OUT))
    return obj


def link(autoconf, rebuilt):
    mk = make_vars(os.path.join(GLES_OBJ, "backend.mk"))
    dist = os.path.join(OUT, "dist")
    maps = os.path.join(OUT, "maps")
    os.makedirs(dist, exist_ok=True)
    os.makedirs(os.path.join(maps, "libGLESv2_gn"), exist_ok=True)
    os.makedirs(os.path.join(maps, "libEGL_gn"), exist_ok=True)
    objs = []
    for listed in objects():
        objs.append(rebuilt.get(listed, os.path.normpath(os.path.join(GLES_OBJ, listed))))
    static = [os.path.normpath(os.path.join(GLES_OBJ, "..", "..", "..", lib.replace("$(DEPTH)/", ""))) for lib in mk.get("STATIC_LIBS", "").split()]
    shared = [os.path.normpath(os.path.join(OBJ, lib.replace("$(DEPTH)/", ""))) for lib in mk.get("SHARED_LIBS", "").split()]
    deffile = os.path.join(ANGLE_SRC, "src", "libGLESv2", "libGLESv2_autogen.def")
    linker = autoconf["LINKER"]
    rsp = os.path.join(OUT, "obj", "libGLESv2.rsp")
    args = ["-NOLOGO", "-DLL", "-OUT:" + os.path.join(dist, "libGLESv2.dll"), "-IMPLIB:" + os.path.join(OUT, "obj", "libGLESv2.lib"),
            "-SUBSYSTEM:WINDOWS,10.0", "-APPCONTAINER", "-OPT:REF,ICF", "-guard:cf,nolongjmp", "-DEF:" + deffile,
            "-MAP:" + os.path.join(maps, "libGLESv2_gn", "libGLESv2.map")]
    args += objs + [os.path.join(GLES_OBJ, "libGLESv2.dll.res")] + static + shared + mk.get("OS_LIBS", "").split()
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"%s"' % a if " " in a else a for a in args) + "\n")
    env = dict(os.environ, LIB=autoconf["LIB"].replace("/", "\\"))
    r = subprocess.run(shlex.split(linker) + ["@" + rsp.replace("\\", "/")], cwd=GLES_OBJ, env=env, capture_output=True, text=True)
    if r.returncode:
        die("linking libGLESv2.dll failed:\n%s%s" % (r.stdout[-4000:], r.stderr[-4000:]))
    for name in ("libEGL.dll", "mozglue.dll"):
        shutil.copyfile(os.path.join(OBJ, "dist", "bin", name), os.path.join(dist, name))
    shutil.copyfile(os.path.join(ANGLE_OBJ, "libEGL_gn", "libEGL.map"), os.path.join(maps, "libEGL_gn", "libEGL.map"))
    size = os.path.getsize(os.path.join(dist, "libGLESv2.dll"))
    print("  linked build/angle/dist/libGLESv2.dll (%d KB), with gecko-w10m's libEGL.dll and mozglue.dll beside it" % (size // 1024))


def main():
    check = "--check" in sys.argv[1:]
    for path, what in ((ANGLE_SRC, "ANGLE's sources (MEGLER_GECKO)"), (GLES_OBJ, "gecko-w10m's ANGLE objects (MEGLER_ANGLE_OBJ)")):
        if not os.path.isdir(path):
            die("no %s at %s" % (what, path))
    autoconf = make_vars(os.path.join(OBJ, "config", "autoconf.mk"))
    for name in ("CXX", "LINKER", "INCLUDE", "LIB"):
        if not autoconf.get(name):
            die("%s is not in %s" % (name, os.path.join(OBJ, "config", "autoconf.mk")))
    changed = apply_patches(check)
    if not changed and not check:
        die("no patch in angle/: nothing to build (gecko-w10m's ANGLE is used as it is)")
    units = units_to_build(changed, check)
    if not units:
        die("no object of libGLESv2.dll has the changed files in it: %s" % ", ".join(changed))
    rebuilt = {}
    for listed, source, unified in units:
        rebuilt[listed] = compile_unit(autoconf, listed, source, unified, changed)
    link(autoconf, rebuilt)


if __name__ == "__main__":
    main()
