"""Build the on-device VideoView test APK using the installed Android SDK."""
import os
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
# The same defaults as tools/build-framework.sh.
sdk = Path(os.environ.get('MEGLER_ANDROID_SDK', str(Path.home()/'Documents')))
jdk = Path(os.environ.get('MEGLER_JDK', 'C:/Program Files/Java/jdk1.8.0_202'))
bt = sdk/'build-tools'/os.environ.get('MEGLER_BUILD_TOOLS_VERSION','29.0.3')
jar = sdk/'platforms'/os.environ.get('MEGLER_ANDROID_PLATFORM','android-29')/'android.jar'
out = root/'build/video-test'
out.mkdir(parents=True, exist_ok=True)
(out/'assets').mkdir(exist_ok=True)
(out/'dex').mkdir(exist_ok=True)
(out/'classes').mkdir(exist_ok=True)   # JDK 8's javac does not make its -d folder
def run(*args):
    subprocess.run([str(x) for x in args], check=True, cwd=root)
common = ['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=480x270:rate=30']
run(*common, '-f','lavfi','-i','sine=frequency=440:sample_rate=44100','-t','2','-c:v','libx264','-preset','fast','-pix_fmt','yuv420p','-profile:v','baseline','-c:a','aac','-ac','2','-movflags','+faststart',out/'assets/av.mp4')
run(*common,'-t','2','-c:v','libx264','-preset','fast','-pix_fmt','yuv420p','-profile:v','baseline','-an','-movflags','+faststart',out/'assets/video-only.mp4')
app = root/'core/tests/app/video'
run(bt/'aapt2.exe','link','-I',jar,'--manifest',app/'AndroidManifest.xml','-A',out/'assets','-o',out/'video.apk')
run(jdk/'bin/javac.exe','-source','8','-target','8','-encoding','UTF-8','-cp',jar,'-d',out/'classes',app/'src/megler/test/video/Main.java')
run(jdk/'bin/jar.exe','cf',out/'classes.jar','-C',out/'classes','.')
run(jdk/'bin/java.exe','-cp',bt/'lib/d8.jar','com.android.tools.r8.D8','--min-api','26','--lib',jar,'--output',out/'dex',out/'classes.jar')
with zipfile.ZipFile(out/'video.apk','a',zipfile.ZIP_DEFLATED) as apk:
    apk.write(out/'dex/classes.dex','classes.dex')
print(out/'video.apk')
