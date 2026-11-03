"""Check SurfaceTexture buffer/canvas lifetime on a JVM, without a phone.

The Canvas fixture counts native owners. This catches per-frame canvas leaks,
checks frame identity through both buffers, and exercises resize and release.
--baseline runs the same checks on HEAD's source (expected to fail pre-fix).
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JDK = Path(os.environ.get('MEGLER_JDK', 'C:/Program Files/Android/Android Studio/jbr'))/'bin'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', action='store_true')
args = parser.parse_args()
out = ROOT/'build/streaming-apps/surface-texture-check'/('baseline' if args.baseline else 'current')
out.mkdir(parents=True, exist_ok=True)
source = 'core/framework/src/android/graphics/SurfaceTexture.java'
surface_source = subprocess.check_output(['git', 'show', 'HEAD:'+source], cwd=ROOT).decode() if args.baseline else (ROOT/source).read_text()
member_path = 'core/framework-overlay/android/graphics/SurfaceTexture.java.members'
members = subprocess.check_output(['git', 'show', 'HEAD:'+member_path], cwd=ROOT).decode() if args.baseline else (ROOT/member_path).read_text()
surface_source = surface_source[:surface_source.rfind('}')] + '\n' + members + '\n}\n'
fixtures = {
    'android/graphics/SurfaceTexture.java': surface_source,
    'android/graphics/Bitmap.java': '''package android.graphics;
public class Bitmap {
 public enum Config { ARGB_8888 }
 public int w,h,frame;
 public static Bitmap createBitmap(int w,int h,Config c){Bitmap b=new Bitmap();b.w=w;b.h=h;return b;}
 public int getWidth(){return w;} public int getHeight(){return h;}
 public void meglerSetRgba(long p){}
}''',
    'android/graphics/Canvas.java': '''package android.graphics;
public class Canvas {
 public static int created,released; private Bitmap b; private boolean gone;
 public Canvas(Bitmap b){this.b=b;created++;}
 public void meglerRelease(){if(!gone){released++;gone=true;}}
 public int save(){if(gone)throw new AssertionError("released canvas reused");return 1;}
 public void restore(){} public void clipRect(Rect r){} public void scale(float a,float b,float c,float d){}
 public int getWidth(){return b.w;} public int getHeight(){return b.h;}
 public void drawColor(int c,PorterDuff.Mode m){b.frame=c;}
 public void drawBitmap(Bitmap b,Rect s,Rect d,Object p){}
}''',
    'android/graphics/Rect.java': 'package android.graphics; public class Rect { public Rect(int a,int b,int c,int d){} }',
    'android/graphics/PorterDuff.java': 'package android.graphics; public class PorterDuff { public enum Mode { CLEAR } }',
    'android/opengl/Matrix.java': 'package android.opengl; public class Matrix { public static void setIdentityM(float[] m,int o){} }',
    'android/os/Looper.java': 'package android.os; public class Looper { public static Looper myLooper(){return null;} public static Looper getMainLooper(){return null;} }',
    'android/os/Handler.java': 'package android.os; public class Handler { public Handler(Looper l){} public void post(Runnable r){r.run();} }',
    'android/view/View.java': 'package android.view; public class View { public void postInvalidate(){} }',
    'SurfaceCheck.java': '''import android.graphics.*;
public class SurfaceCheck {
 static void check(boolean b,String why){if(!b)throw new AssertionError(why);}
 public static void main(String[] args){
  SurfaceTexture t=new SurfaceTexture(0);t.setDefaultBufferSize(32,64);
  for(int i=0;i<240;i++){
   Canvas c=t.meglerLock(null);check(c.getWidth()==32&&c.getHeight()==64,"wrong buffer size");
   c.drawColor(i,PorterDuff.Mode.CLEAR);t.meglerPost(c);
   check(t.meglerFront().frame==i,"wrong frame published");
   check(Canvas.created<=2,"one canvas per buffer exceeded at frame "+i+": "+Canvas.created);
  }
  check(Canvas.created==2&&Canvas.released==0,"steady playback recreated canvases");
  t.setDefaultBufferSize(64,32);
  for(int i=0;i<2;i++){
   Canvas c=t.meglerLock(null);check(c.getWidth()==64&&c.getHeight()==32,"resize lost");t.meglerPost(c);
  }
  check(Canvas.created==4&&Canvas.released==2,"resize did not retire both old canvases");
  Bitmap external=Bitmap.createBitmap(64,32,Bitmap.Config.ARGB_8888);external.frame=777;
  check(t.meglerPostBitmap(external)&&t.meglerFront().frame==777,"direct bitmap lost");
  check(Canvas.created==Canvas.released,"direct bitmap retained old canvases");
  for(int i=0;i<4;i++){
   Canvas c=t.meglerLock(null);c.drawColor(900+i,PorterDuff.Mode.CLEAR);t.meglerPost(c);
   check(t.meglerFront().frame==900+i,"canvas does not belong to its buffer after direct bitmap");
  }
  Canvas locked=t.meglerLock(null);int released=Canvas.released;t.release();
  check(Canvas.released==released,"release freed a canvas while the producer was drawing");
  locked.drawColor(1000,PorterDuff.Mode.CLEAR);t.meglerPost(locked);t.release();
  check(Canvas.created==Canvas.released,"release leaked canvases");
  check(t.meglerLock(null)==null,"released texture accepted a producer");
  System.out.println("240 frames, two canvases; resize, direct bitmap/canvas interchange and release passed");
 }
}''',
}
for name, text in fixtures.items():
    path = out/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
classes = out/'classes'
classes.mkdir(exist_ok=True)
subprocess.run([str(JDK/'javac.exe'), '-encoding', 'UTF-8', '-d', str(classes), *[str(out/name) for name in fixtures]], check=True)
subprocess.run([str(JDK/'java.exe'), '-cp', str(classes), 'SurfaceCheck'], check=True)
