#!/usr/bin/env python3
"""Builds a test app as Google Play has an app built from an Android App Bundle: a base APK and split APKs, and bundles
them as they go about outside Google Play -- an .apks in bundletool's layout and an .xapk in APKPure's. Called by
tools/build-test-app.sh for an app folder that has a file named `splits`, which says what the splits are (that file's own
comment says how). Like the other test apps, nothing is signed.

    build-split-app.py <app folder> <out folder> <build-tools> <android.jar> <jdk> <ndk clang | ->

    <out>/<name>.apks       splits/base-master.apk, splits/base-<config or abi>.apk, splits/<feature>-master.apk,
                            standalones/standalone-armeabi_v7a.apk (a copy of the base, which a bundle of bundletool's has
                            for Android versions without splits), toc.pb (not bundletool's: some bytes, which nothing reads)
    <out>/<name>.xapk       <package>.apk, config.<config or abi>.apk, <feature>.apk, icon.png, manifest.json (with the
                            splits and the expansion files), Android/obb/<package>/<obb>
    <out>/<name>-base.apk   the base alone

aapt2 makes the config splits (link --split) and names them as bundletool does (config.xxhdpi, config.en: with the
minimum SDK at 21 it gives a density no API level). The splits for a processor, the feature split and the asset pack have
manifests written here and linked by aapt2.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

app, out, bt, jar, jdk, ndkcc = sys.argv[1:7]
name = os.path.basename(os.path.normpath(app))
work = os.path.join(out, name + '.tmp')
exe = '.exe' if os.name == 'nt' else ''
aapt2 = os.path.join(bt, 'aapt2' + exe)
d8 = os.path.join(bt, 'lib', 'd8.jar')
javac = os.path.join(jdk, 'bin', 'javac' + exe)
java = os.path.join(jdk, 'bin', 'java' + exe)
jartool = os.path.join(jdk, 'bin', 'jar' + exe)
sep = ';' if os.name == 'nt' else ':'


def run(*args, quiet_compressed=False):
    p = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    text = '\n'.join(l for l in p.stdout.splitlines() if not (quiet_compressed and 'is compressed' in l))
    if p.returncode != 0:
        sys.stderr.write(text + '\n')
        sys.exit('%s failed for %s' % (os.path.basename(str(args[0])), name))
    return text


spec = {'config': [], 'abi': [], 'feature': [], 'assets': [], 'obb': []}
real_abi = None
for line in open(os.path.join(app, 'splits'), encoding='utf-8'):
    words = line.split('#', 1)[0].split()
    if not words:
        continue
    spec[words[0]].append(words[1])
    if words[0] == 'abi' and 'real' in words[2:]:
        real_abi = words[1]

manifest = open(os.path.join(app, 'AndroidManifest.xml'), encoding='utf-8').read()
package = re.search(r'package="([^"]+)"', manifest).group(1)
version_code = re.search(r'android:versionCode="(\d+)"', manifest).group(1)
version_name = re.search(r'android:versionName="([^"]+)"', manifest).group(1)

shutil.rmtree(work, ignore_errors=True)
for d in ('gen', 'classes', 'dex', 'lib', 'parts', 'm'):
    os.makedirs(os.path.join(work, d))

# The native libraries first, as the other test apps': armeabi-v7a, Thumb-2, softfp, by the NDK.
libs = []
if os.path.isdir(os.path.join(app, 'jni')):
    if ndkcc == '-':
        sys.exit('no NDK: %s has native code' % name)
    for src in sorted(os.listdir(os.path.join(app, 'jni'))):
        if not src.endswith('.c'):
            continue
        lib = 'lib' + src[:-2] + '.so'
        run(ndkcc, '--target=armv7a-linux-androideabi21', '-mthumb', '-O2', '-fPIC', '-shared', '-Wall', '-Wextra',
            '-Werror=implicit-function-declaration', '-Wl,-soname,' + lib, '-o', os.path.join(work, 'lib', lib),
            os.path.join(app, 'jni', src), '-llog', '-landroid', '-ljnigraphics', '-ldl')
        libs.append(lib)

# The base and its config splits, in one link.
run(aapt2, 'compile', '--dir', os.path.join(app, 'res'), '-o', os.path.join(work, 'res.zip'))
link = [aapt2, 'link', '-o', os.path.join(work, 'parts', 'base.apk'), '-I', jar, '--manifest', os.path.join(app, 'AndroidManifest.xml'),
        '--java', os.path.join(work, 'gen'), '--min-sdk-version', '21', '--target-sdk-version', '28']
if os.path.isdir(os.path.join(app, 'assets')):
    link += ['-A', os.path.join(app, 'assets')]
for q in spec['config']:
    link += ['--split', os.path.join(work, 'parts', 'config.%s.apk' % q) + sep + q]
run(*(link + [os.path.join(work, 'res.zip')]), quiet_compressed=True)

# The base's code.
sources = [os.path.join(r, f) for top in (os.path.join(app, 'src'), os.path.join(work, 'gen')) for r, _, fs in os.walk(top) for f in fs if f.endswith('.java')]
boot = jar + sep + os.path.join(bt, 'core-lambda-stubs.jar')
run(javac, '-nowarn', '-source', '8', '-target', '8', '-encoding', 'UTF-8', '-bootclasspath', boot, '-d', os.path.join(work, 'classes'), *sources)
run(jartool, 'cf', os.path.join(work, 'classes.jar'), '-C', os.path.join(work, 'classes'), '.')
run(java, '-cp', d8, 'com.android.tools.r8.D8', '--release', '--min-api', '26', '--lib', jar, '--output', os.path.join(work, 'dex'), os.path.join(work, 'classes.jar'))
with zipfile.ZipFile(os.path.join(work, 'parts', 'base.apk'), 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(work, 'dex', 'classes.dex'), 'classes.dex')


def split_manifest(split, feature=False, code=False):
    path = os.path.join(work, 'm', split + '.xml')
    extra = ' android:isFeatureSplit="true"' if feature else ''
    open(path, 'w', encoding='utf-8').write(
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="%s" split="%s" android:versionCode="%s"%s>'
        '<application android:hasCode="%s"/></manifest>\n' % (package, split, version_code, extra, 'true' if code else 'false'))
    return path


def link_split(split, out_name, feature=False, code=False):
    run(aapt2, 'link', '-o', os.path.join(work, 'parts', out_name), '-I', jar, '--manifest', split_manifest(split, feature, code), quiet_compressed=True)
    return os.path.join(work, 'parts', out_name)


# A split for each processor: the libraries, under that processor's folder.
for abi in spec['abi']:
    apk = link_split('config.' + abi, 'config.%s.apk' % abi)
    with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
        for lib in libs:
            z.write(os.path.join(work, 'lib', lib), 'lib/%s/%s' % (abi.replace('_', '-'), lib))

# Feature splits, with code of their own compiled against the base's.
for feature in spec['feature']:
    src = os.path.join(app, 'split', feature, 'src')
    fsources = [os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs if f.endswith('.java')]
    fclasses = os.path.join(work, 'classes-' + feature)
    fdex = os.path.join(work, 'dex-' + feature)
    os.makedirs(fclasses)
    os.makedirs(fdex)
    run(javac, '-nowarn', '-source', '8', '-target', '8', '-encoding', 'UTF-8', '-bootclasspath', boot, '-cp', os.path.join(work, 'classes'),
        '-d', fclasses, *fsources)
    run(jartool, 'cf', fclasses + '.jar', '-C', fclasses, '.')
    run(java, '-cp', d8, 'com.android.tools.r8.D8', '--release', '--min-api', '26', '--lib', jar, '--lib', os.path.join(work, 'classes.jar'),
        '--output', fdex, fclasses + '.jar')
    apk = link_split(feature, feature + '.apk', feature=True, code=True)
    with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(fdex, 'classes.dex'), 'classes.dex')

# Asset packs: assets alone. (Put in here rather than by aapt2's -A, which on Windows names a file in a subfolder of the
# assets "assets/pack\hello.txt", with a backslash: no APK has such a name, and no reader finds it.)
for pack in spec['assets']:
    apk = link_split(pack, pack + '.apk')
    top = os.path.join(app, 'split', pack, 'assets')
    with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
        for r, _, fs in os.walk(top):
            for f in sorted(fs):
                z.write(os.path.join(r, f), 'assets/' + os.path.relpath(os.path.join(r, f), top).replace(os.sep, '/'))

parts = os.path.join(work, 'parts')
configs = ['config.' + q for q in spec['config']] + ['config.' + a for a in spec['abi']]

# bundletool's layout, deflated (as BiP's .apks is).
apks = os.path.join(out, name + '.apks')
with zipfile.ZipFile(apks, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('toc.pb', b'\x0a\x04test')
    z.write(os.path.join(parts, 'base.apk'), 'splits/base-master.apk')
    for c in configs:
        z.write(os.path.join(parts, c + '.apk'), 'splits/base-%s.apk' % c[len('config.'):])
    for f in spec['feature'] + spec['assets']:
        z.write(os.path.join(parts, f + '.apk'), 'splits/%s-master.apk' % f)
    z.write(os.path.join(parts, 'base.apk'), 'standalones/standalone-armeabi_v7a.apk')

# APKPure's, stored (as MangaDex's .xapk is), with an icon and an expansion file.
icon = None
for r, _, fs in os.walk(os.path.join(app, 'res')):
    if 'icon.png' in fs:
        icon = os.path.join(r, 'icon.png')
xapk = os.path.join(out, name + '.xapk')
split_apks = [{'file': package + '.apk', 'id': 'base'}] + [{'file': c + '.apk', 'id': c} for c in configs] + \
             [{'file': f + '.apk', 'id': f} for f in spec['feature'] + spec['assets']]
expansions = [{'file': 'Android/obb/%s/%s' % (package, o), 'install_location': 'EXTERNAL_STORAGE', 'install_path': 'Android/obb/%s/%s' % (package, o)}
              for o in spec['obb']]
info = {'xapk_version': 2, 'package_name': package, 'name': name, 'version_code': version_code, 'version_name': version_name,
        'min_sdk_version': '21', 'target_sdk_version': '28', 'split_configs': configs, 'split_apks': split_apks, 'expansions': expansions}
if icon:
    info['icon'] = 'icon.png'
with zipfile.ZipFile(xapk, 'w', zipfile.ZIP_STORED) as z:
    z.write(os.path.join(parts, 'base.apk'), package + '.apk')
    for c in configs:
        z.write(os.path.join(parts, c + '.apk'), c + '.apk')
    for f in spec['feature'] + spec['assets']:
        z.write(os.path.join(parts, f + '.apk'), f + '.apk')
    if icon:
        z.write(icon, 'icon.png')
    z.writestr('manifest.json', json.dumps(info, indent=1))
    for o in spec['obb']:
        z.write(os.path.join(app, 'obb', o), 'Android/obb/%s/%s' % (package, o))

shutil.copyfile(os.path.join(parts, 'base.apk'), os.path.join(out, name + '-base.apk'))
shutil.rmtree(work)
print('built build/guest/java/%s.apks and %s.xapk (%d and %d bytes; base and %d splits: %s)' % (
    name, name, os.path.getsize(apks), os.path.getsize(xapk), len(configs) + len(spec['feature']) + len(spec['assets']),
    ', '.join(configs + spec['feature'] + spec['assets'])))
