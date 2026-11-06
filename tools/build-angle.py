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

    python tools/build-angle.py --arch x64|x86|arm64

The same ANGLE for desktop Windows on that processor (docs/DESKTOP.md), into
build/angle/<arch>/dist: libGLESv2.dll and libEGL.dll, each from every object
gecko-w10m's build made of them -- the objects its link lists, and those of
the static libraries it links -- compiled again from its sources with Megler's
patches, with the flags gecko-w10m's build used for each, but for that
processor, with no Gecko in them: no mozilla-config.h, no STL wrappers and
mozglue allocator (the C runtime's, linked statically, so the DLLs need no
runtime DLL beside them), and Megler's zlib (core/third_party/zlib) for
Gecko's. For an app container, as the phone's are (ANGLE_ENABLE_WINDOWS_UWP).
d3dcompiler_47.dll is the system's.

Environment: MEGLER_GECKO (gecko-w10m's checkout, ../gecko-w10m by default),
MEGLER_ANGLE_OBJ (its object directory, C:\rw-obj by default), MEGLER_LLVM
(clang-cl and lld-link, C:\Program Files\LLVM\bin by default), MEGLER_JOBS
(compilers at once, the processors by default).
"""
import concurrent.futures
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


# ------------------------------------------------------------------ desktop Windows

# Megler's arch name: clang's target, the libraries' folder name, the linker's machine.
ARCHES = {
    "x64": ("x86_64-pc-windows-msvc", "x64", "X64"),
    "x86": ("i686-pc-windows-msvc", "x86", "X86"),
    "arm64": ("aarch64-pc-windows-msvc", "arm64", "ARM64"),
}
LLVM = os.environ.get("MEGLER_LLVM") or "C:\\Program Files\\LLVM\\bin"
ZLIB = os.path.join(ROOT, "core", "third_party", "zlib")


def depth_path(lib):
    return os.path.normpath(lib.replace("$(DEPTH)", OBJ))


def listed(path):
    with open(path, encoding="utf-8") as f:
        return [os.path.normpath(os.path.join(os.path.dirname(path), line.strip())) for line in f if line.strip()]


def dll_objects(dll_dir, dll_list):
    """Every object a DLL of gecko-w10m's was linked from: its own list's, and those of the static libraries it links."""
    objs = listed(os.path.join(dll_dir, dll_list))
    for lib in make_vars(os.path.join(dll_dir, "backend.mk")).get("STATIC_LIBS", "").split():
        lib = depth_path(lib)
        if os.path.basename(lib) == "pure_virtual.lib":
            continue                  # Gecko's own _purecall; the C runtime has one
        objs += listed(os.path.splitext(lib)[0] + "_lib.list")
    seen, out = set(), []
    for o in objs:
        if norm(o) not in seen:
            seen.add(norm(o))
            out.append(o)
    return out


def source_of(obj, topsrc):
    """The source an object of gecko-w10m's build was compiled from, and whether it is a unified file of its object directory."""
    d, base = os.path.dirname(obj), os.path.splitext(os.path.basename(obj))[0]
    for ext in (".cpp", ".c"):
        if base.startswith("Unified_") and os.path.exists(os.path.join(d, base + ext)):
            return os.path.join(d, base + ext), True
    mk = make_vars(os.path.join(d, "backend.mk"))
    for src in (mk.get("CPPSRCS", "") + " " + mk.get("CSRCS", "")).split():
        if src.startswith("$(topsrcdir)") and os.path.splitext(os.path.basename(src))[0] == base:
            return os.path.normpath(src.replace("$(topsrcdir)", topsrc)), False
    die("no source for %s in %s" % (obj, os.path.join(d, "backend.mk")))


def desktop_flags(obj_dir, cpp):
    """The flags gecko-w10m's build compiled a directory's sources with, less what is Gecko's or 32-bit ARM's."""
    flags = shlex.split(make_vars(os.path.join(obj_dir, "backend.mk")).get("COMPUTED_CXXFLAGS" if cpp else "COMPUTED_CFLAGS", ""))
    dist = norm(os.path.join(OBJ, "dist"))
    out, skip = [], False
    for f in flags:
        if skip:
            skip = False
            continue
        if f == "-FI":                 # -FI C:/rw-obj/mozilla-config.h
            skip = True
            continue
        if f in ("-guard:cf", "-DMOZILLA_CLIENT", "-FIgecko_w10m_arm_intrin.h", "-DGECKO_W10M=1", "-DU_PLATFORM_HAS_WINUWP_API=1"):
            continue
        # dist/stl_wrappers puts the STL's allocations on mozglue's allocator; dist/include has Gecko's zlib.h.
        if f.startswith("-I") and norm(f[2:]).startswith(dist):
            continue
        out.append("-MT" if f == "-MD" else f)
    return out


def desktop_compile(job):
    target, src, obj, flags = job
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    cmd = [os.path.join(LLVM, "clang-cl.exe"), "--target=" + target, "-fms-compatibility-version=19.44"] + flags + ["-c", "-Fo" + obj, src]
    r = subprocess.run(cmd, cwd=os.path.dirname(obj), capture_output=True, text=True)
    return obj, r.returncode, (r.stdout + r.stderr)[-6000:]


def desktop_units(arch, objs, changed, topsrc, done):
    """(clang's target, source, object, flags) for each object of a DLL; Gecko's zlib becomes Megler's. `done` is shared between the DLLs."""
    target = ARCHES[arch][0]
    out_root = os.path.join(OUT, arch, "obj")
    patched = {norm(os.path.join(ANGLE_SRC, c)): os.path.join(OUT, "src", c) for c in changed}
    zlib_dir = norm(os.path.join(OBJ, "modules", "zlib", "src"))
    jobs, result = [], []
    for o in objs:
        if norm(os.path.dirname(o)) == zlib_dir:
            if "zlib" not in done:
                done["zlib"] = []
                for name in sorted(os.listdir(ZLIB)):
                    if name.endswith(".c") and not name.startswith("gz"):
                        obj = os.path.join(out_root, "zlib", name[:-2] + ".obj")
                        jobs.append((target, os.path.join(ZLIB, name), obj, ["-MT", "-O2", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-I" + ZLIB]))
                        done["zlib"].append(obj)
                result += done["zlib"]
            continue
        rel = os.path.relpath(os.path.dirname(o), OBJ)
        obj = os.path.join(out_root, rel, os.path.splitext(os.path.basename(o))[0] + ".obj")
        result.append(obj)
        if norm(obj) in done:
            continue
        done[norm(obj)] = True
        src, unified = source_of(o, topsrc)
        cpp = src.endswith(".cpp") or src.endswith(".cc")
        if unified:
            # Its includes are relative to gecko-w10m's object directory: made absolute, and the patched files Megler's copies.
            lines = []
            with open(src, encoding="utf-8") as f:
                for line in f:
                    m = re.match(r'(\s*#include ")([^"]+)(".*)', line.rstrip("\n"))
                    if m:
                        to = os.path.normpath(os.path.join(os.path.dirname(src), m.group(2)))
                        to = patched.get(norm(to), to)
                        line = m.group(1) + to.replace("\\", "/") + m.group(3) + "\n"
                    lines.append(line)
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            src = os.path.splitext(obj)[0] + os.path.splitext(src)[1]
            with open(src, "w", encoding="utf-8") as f:
                f.writelines(lines)
        else:
            src = patched.get(norm(src), src)
        flags = desktop_flags(os.path.dirname(o), cpp)
        flags = (["-std:c++20"] if cpp else []) + flags + ["-I" + os.path.dirname(o).replace("\\", "/"), "-I" + ZLIB.replace("\\", "/")]
        if cpp and os.path.basename(os.path.dirname(o)) == "angle_d3d11_backend_gn":
            flags.append("-FI" + windows_h(out_root))
        jobs.append((target, src, obj, flags))
    return jobs, result


def windows_h(out_root):
    """
    What Gecko's windows.h (dist/stl_wrappers) does that the Direct3D 11 back end counts on: gecko-w10m's
    NativeWindow11WinRT.cpp includes the Windows Runtime's XAML headers, which have a method GetCurrentTime, and winbase.h's
    macro of that name is undefined there.
    """
    path = os.path.join(out_root, "windows_undef.h")
    os.makedirs(out_root, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("/* tools/build-angle.py: what Gecko's windows.h wrapper undefines for the Direct3D 11 back end. */\n"
                "#include <windows.h>\n#undef GetCurrentTime\n")
    return path.replace("\\", "/")


def desktop_link(arch, autoconf, name, objs, deffile, res, os_libs, extra):
    _, vcarch, machine = ARCHES[arch]
    dist = os.path.join(OUT, arch, "dist")
    os.makedirs(dist, exist_ok=True)
    # The libraries' folders of the toolset and the SDK gecko-w10m built with, for this processor.
    lib = ";".join(re.sub(r"[/\\]arm[/\\]?$", "/" + vcarch, p) for p in autoconf["LIB"].split(";"))
    rsp = os.path.join(OUT, arch, "obj", name + ".rsp")
    args = ["-NOLOGO", "-DLL", "-OUT:" + os.path.join(dist, name + ".dll"), "-IMPLIB:" + os.path.join(OUT, arch, "obj", name + ".lib"),
            "-MACHINE:" + machine, "-SUBSYSTEM:WINDOWS,10.0", "-APPCONTAINER", "-OPT:REF,ICF", "-DEF:" + deffile]
    args += objs + extra + [res] + os_libs
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"%s"' % a if " " in a else a for a in args) + "\n")
    r = subprocess.run([os.path.join(LLVM, "lld-link.exe"), "@" + rsp], env=dict(os.environ, LIB=lib.replace("/", "\\")), capture_output=True, text=True)
    if r.returncode:
        die("linking %s.dll for %s failed:\n%s%s" % (name, arch, r.stdout[-6000:], r.stderr[-6000:]))
    print("  linked build/angle/%s/dist/%s.dll (%d KB)" % (arch, name, os.path.getsize(os.path.join(dist, name + ".dll")) // 1024))


def build_desktop(arch):
    autoconf = make_vars(os.path.join(OBJ, "config", "autoconf.mk"))
    topsrc = os.path.join(GECKO, "engine", "firefox")
    egl_obj = os.path.join(ANGLE_OBJ, "libEGL_gn")
    changed = apply_patches(False)
    done = {}
    gles_jobs, gles = desktop_units(arch, dll_objects(GLES_OBJ, "libGLESv2_dll.list"), changed, topsrc, done)
    egl_jobs, egl = desktop_units(arch, dll_objects(egl_obj, "libEGL_dll.list"), changed, topsrc, done)
    jobs = gles_jobs + egl_jobs
    print("  %d objects for libGLESv2.dll, %d for libEGL.dll: %d to compile for %s" % (len(gles), len(egl), len(jobs), arch))
    os.environ["INCLUDE"] = autoconf["INCLUDE"].replace("/", "\\")
    failed = []
    workers = int(os.environ.get("MEGLER_JOBS") or os.cpu_count() or 4)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for obj, code, text in pool.map(desktop_compile, jobs):
            if code:
                failed.append((obj, text))
    for obj, text in failed[:3]:
        print("  FAILED %s:\n%s" % (os.path.relpath(obj, OUT), text))
    if failed:
        die("%d of %d objects did not compile for %s" % (len(failed), len(jobs), arch))
    print("  compiled %d objects" % len(jobs))
    gles_mk = make_vars(os.path.join(GLES_OBJ, "backend.mk"))
    egl_mk = make_vars(os.path.join(egl_obj, "backend.mk"))
    # (The Windows Runtime's activation and strings, which Trim11 and the window classes call: Gecko's link had them elsewhere.)
    desktop_link(arch, autoconf, "libGLESv2", gles, os.path.join(ANGLE_SRC, "src", "libGLESv2", "libGLESv2_autogen.def"),
                 os.path.join(GLES_OBJ, "libGLESv2.dll.res"), gles_mk.get("OS_LIBS", "").split() + ["runtimeobject.lib"], [])
    desktop_link(arch, autoconf, "libEGL", egl, os.path.join(ANGLE_SRC, "src", "libEGL", "libEGL_autogen.def"),
                 os.path.join(egl_obj, "libEGL.dll.res"), egl_mk.get("OS_LIBS", "").split(), [os.path.join(OUT, arch, "obj", "libGLESv2.lib")])


def main():
    if "--arch" in sys.argv[1:]:
        at = sys.argv.index("--arch")
        arch = sys.argv[at + 1] if at + 1 < len(sys.argv) else ""
        if arch not in ARCHES:
            die("--arch x64, x86 or arm64, not %s" % (arch or "nothing"))
        for path, what in ((ANGLE_SRC, "ANGLE's sources (MEGLER_GECKO)"), (GLES_OBJ, "gecko-w10m's ANGLE objects (MEGLER_ANGLE_OBJ)")):
            if not os.path.isdir(path):
                die("no %s at %s" % (what, path))
        build_desktop(arch)
        return
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
