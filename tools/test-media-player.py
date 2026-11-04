"""Exercise the real MediaPlayer overlay on a JVM with deterministic host doubles.
Run build-framework.sh first to generate its source lists. This does not validate
Media Foundation, native pixels, the Dalvik runtime, or a phone's decoders.
"""
import os
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
jdk = pathlib.Path(os.environ.get('MEGLER_JDK', r'C:\Program Files\Java\jdk1.8.0_202'))
out = root / 'build/player-tests'
out.mkdir(parents=True, exist_ok=True)
for name in ('stubs', 'framework', 'mocks'):
    (out/name).mkdir(exist_ok=True)   # JDK 8's javac does not make its -d folder

def run(*args):
    subprocess.run([str(x) for x in args], cwd=root, check=True)

run(jdk/'bin/javac.exe', '-nowarn', '-source', '8', '-target', '8', '-encoding', 'UTF-8', '-d', out/'stubs', '@build/framework/stubs.arg')
run(jdk/'bin/javac.exe', '-nowarn', '-source', '8', '-target', '8', '-encoding', 'UTF-8', '-cp', out/'stubs', '-d', out/'framework', '@build/framework/sources.arg')
files = {
    'android/os/Looper.java': 'package android.os; public class Looper { public static Looper myLooper(){return new Looper();} public static Looper getMainLooper(){return new Looper();} }',
    'android/os/Handler.java': '''package android.os; public class Handler {
static final java.util.concurrent.ExecutorService e=java.util.concurrent.Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r);t.setDaemon(true);return t;});
public Handler(Looper l){} public boolean post(Runnable r){e.execute(r);return true;}
}''',
    'android/graphics/Bitmap.java': 'package android.graphics; public class Bitmap { public enum Config {ARGB_8888} public static Bitmap createBitmap(int w,int h,Config c){return new Bitmap();} public void recycle(){} }',
    'android/graphics/RectF.java': 'package android.graphics; public class RectF {public RectF(float a,float b,float c,float d){} }',
    'android/graphics/Canvas.java': 'package android.graphics; public class Canvas { public int getWidth(){return 640;} public int getHeight(){return 480;} public void drawColor(int c){} public void drawBitmap(Bitmap b,Rect r,RectF f,Paint p){} }',
    'android/view/Surface.java': 'package android.view; public class Surface {public volatile int shown; public boolean isValid(){return true;} public android.graphics.Canvas lockCanvas(android.graphics.Rect r){return new android.graphics.Canvas();} public void unlockCanvasAndPost(android.graphics.Canvas c){shown++;} }',
    'megler/Audio.java': r'''package megler;
import java.util.concurrent.*;
public class Audio {
static class D {int mode,audio,video;D(int m){mode=m;}}
static class T {long ns,elapsed;int total;boolean play;}
static final ConcurrentHashMap<Long,D> ds=new ConcurrentHashMap<>();
static final ConcurrentHashMap<Integer,T> ts=new ConcurrentHashMap<>();
static long serial;static int tid;public static volatile int closed;public static volatile boolean delay;
public static final CountDownLatch entered=new CountDownLatch(1),unblock=new CountDownLatch(1);
public static long decOpen(byte[] data){if(delay){entered.countDown();try{unblock.await();}catch(Exception e){throw new RuntimeException(e);}}synchronized(Audio.class){long id=++serial;ds.put(id,new D(data[0]));return id;}}
public static long decOpenFd(int f,long o,long n){return decOpen(new byte[]{1});}
static D d(long h){D d=ds.get(h);if(d==null)throw new AssertionError("decoder used after close");return d;}
public static void decInfo(long h,int[] i){D d=d(h);i[0]=d.mode==2?0:48000;i[1]=d.mode==2?0:2;i[2]=120;if(i.length>=5){i[3]=d.mode==1?0:320;i[4]=d.mode==1?0:240;}}
public static int decRead(long h,short[] b){D d=d(h);int n=Math.min(b.length,11520-d.audio);d.audio+=n;return n;}
public static int decVideo(long h,android.graphics.Bitmap b,long[] pts){D d=d(h);if(d.video>=3)return 0;pts[0]=d.video++*40000;return 1;}
public static void decSeek(long h,int ms){D d=d(h);d.audio=Math.min(11520,ms*96);d.video=Math.min(3,ms/40);}
public static void decClose(long h){if(ds.remove(h)==null)throw new AssertionError("double close");closed++;}
public static int trackOpen(int r,int c){T t=new T();int id=++tid;ts.put(id,t);return id;}
static T t(int h){T t=ts.get(h);if(t==null)throw new AssertionError("track used after close");return t;}
static long pos(T t){return (t.elapsed+(t.play?System.nanoTime()-t.ns:0))*48000/1000000000;}
public static int trackWrite(int h,short[] b,int o,int n,boolean block){T t=t(h);int free=Math.max(0,4800-(t.total-(int)pos(t)));int got=Math.min(n,free*2);t.total+=got/2;return got;}
public static void trackPlay(int h,boolean p){T t=t(h);if(t.play)t.elapsed+=System.nanoTime()-t.ns;t.ns=System.nanoTime();t.play=p;}
public static void trackFlush(int h){T t=t(h);t.elapsed=0;t.ns=System.nanoTime();t.total=0;}
public static void trackClose(int h){if(ts.remove(h)==null)throw new AssertionError("double track close");}
public static long trackPosition(int h){return pos(t(h));}
public static int trackPending(int h){T t=t(h);return Math.max(0,t.total-(int)pos(t));}
public static void trackVolume(int h,float l,float r){}
public static int fdOf(java.io.FileDescriptor f){return -1;}
public static byte[] readAll(java.io.InputStream in)throws java.io.IOException{return new byte[]{1};}
public static int live(){return ds.size()+ts.size();}
}''',
    'android/view/View.java': '''package android.view; public class View { public android.content.Context getContext(){return null;} public void requestLayout(){} public boolean post(Runnable r){return new android.os.Handler(null).post(r);} public static int getDefaultSize(int d,int s){return d;} }''',
    'android/net/Uri.java': '''package android.net; public class Uri {private java.net.URI u; public static Uri parse(String s){Uri r=new Uri();r.u=java.net.URI.create(s);return r;} public String getScheme(){return u.getScheme();} public String getPath(){return u.getPath();} public String toString(){return u.toString();}}''',
    'android/view/SurfaceView.java': '''package android.view; public class SurfaceView extends View {
public SurfaceView(android.content.Context c,android.util.AttributeSet a,int s){} public SurfaceView(android.content.Context c,android.util.AttributeSet a,int s,int r){}
final H h=new H(); public SurfaceHolder getHolder(){return h;} public void createSurface(){for(SurfaceHolder.Callback c:h.cs)c.surfaceCreated(h);} public void destroySurface(){for(SurfaceHolder.Callback c:h.cs)c.surfaceDestroyed(h);}
static class H implements SurfaceHolder { final Surface s=new Surface();final java.util.List<Callback> cs=new java.util.ArrayList<>();public void addCallback(Callback c){cs.add(c);} public void removeCallback(Callback c){cs.remove(c);} public boolean isCreating(){return false;} public void setType(int t){} public void setFixedSize(int w,int h){} public void setSizeFromLayout(){} public void setFormat(int f){} public void setKeepScreenOn(boolean b){} public android.graphics.Canvas lockCanvas(){return new android.graphics.Canvas();} public android.graphics.Canvas lockCanvas(android.graphics.Rect r){return lockCanvas();} public void unlockCanvasAndPost(android.graphics.Canvas c){} public android.graphics.Rect getSurfaceFrame(){return null;} public Surface getSurface(){return s;} }
}''',
    'TestPlayer.java': r'''import android.media.MediaPlayer;import java.util.concurrent.*;
public class TestPlayer {
static int passed;static void check(boolean b,String s){if(!b)throw new AssertionError(s);passed++;System.out.println("PASS "+s);}
static MediaPlayer source(int mode)throws Exception{java.io.File f=java.io.File.createTempFile("megler", ".bin");f.deleteOnExit();try(java.io.FileOutputStream o=new java.io.FileOutputStream(f)){o.write(mode);}MediaPlayer p=new MediaPlayer();p.setDataSource(f.getPath());p.prepare();return p;}
public static void main(String[] args)throws Exception{
Thread.setDefaultUncaughtExceptionHandler((t,e)->{e.printStackTrace();System.exit(1);});
for(int mode=1;mode<=3;mode++){
MediaPlayer p=source(mode);android.view.Surface s=new android.view.Surface();p.setSurface(s);CountDownLatch done=new CountDownLatch(1);p.setOnCompletionListener(mp->done.countDown());p.start();check(done.await(3,TimeUnit.SECONDS),"completion mode "+mode);check(mode==1?s.shown==0:s.shown==3,"frames mode "+mode);p.release();check(megler.Audio.live()==0,"resources mode "+mode);
}
MediaPlayer p=source(2);p.start();Thread.sleep(25);p.pause();int before=p.getCurrentPosition();Thread.sleep(50);check(p.getCurrentPosition()==before,"video pause freezes clock");CountDownLatch seek=new CountDownLatch(1);p.setOnSeekCompleteListener(mp->seek.countDown());p.seekTo(40);check(seek.await(1,TimeUnit.SECONDS),"seek while paused");check(p.getCurrentPosition()==40,"paused seek position");p.start();Thread.sleep(200);p.start();check(p.isPlaying(),"restart after completion");p.release();
p=source(1);p.setLooping(true);p.start();Thread.sleep(300);check(p.isPlaying(),"audio loops");check(p.getCurrentPosition()<150,"loop resets clock");p.release();
for(int i=0;i<30;i++){p=source(3);p.start();p.stop();p.prepare();p.start();p.release();}Thread.sleep(50);check(megler.Audio.live()==0,"rapid stop prepare start release");
megler.Audio.delay=true;MediaPlayer old=new MediaPlayer();java.io.File f=java.io.File.createTempFile("megler", ".bin");try(java.io.FileOutputStream o=new java.io.FileOutputStream(f)){o.write(1);}old.setDataSource(f.getPath());java.util.concurrent.atomic.AtomicInteger events=new java.util.concurrent.atomic.AtomicInteger();old.setOnPreparedListener(mp->events.incrementAndGet());int closed=megler.Audio.closed;old.prepareAsync();check(megler.Audio.entered.await(1,TimeUnit.SECONDS),"async prepare in flight");old.release();megler.Audio.unblock.countDown();long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);while(megler.Audio.closed==closed&&System.nanoTime()<end)Thread.sleep(5);Thread.sleep(30);check(events.get()==0,"stale prepared suppressed");check(megler.Audio.live()==0,"cancelled prepare closes decoder");megler.Audio.delay=false;f.delete();
com.sun.net.httpserver.HttpServer server=com.sun.net.httpserver.HttpServer.create(new java.net.InetSocketAddress("127.0.0.1",0),0);java.util.concurrent.atomic.AtomicBoolean header=new java.util.concurrent.atomic.AtomicBoolean();server.createContext("/media",e->{header.set("secret".equals(e.getRequestHeaders().getFirst("X-Megler-Test")));e.sendResponseHeaders(200,1);e.getResponseBody().write(1);e.close();});server.start();try{MediaPlayer net=new MediaPlayer();java.util.Map<String,String> headers=new java.util.HashMap<>();headers.put("X-Megler-Test","secret");net.setDataSource("http://127.0.0.1:"+server.getAddress().getPort()+"/media",headers);check(!header.get(),"URL deferred to preparation");net.prepare();check(header.get(),"HTTP headers reach server");net.release();}finally{server.stop(0);}
java.io.File vf=java.io.File.createTempFile("megler", ".bin");try(java.io.FileOutputStream o=new java.io.FileOutputStream(vf)){o.write(2);}android.widget.VideoView view=new android.widget.VideoView(null);CountDownLatch vd=new CountDownLatch(1);view.setOnCompletionListener(mp->vd.countDown());view.setVideoURI(android.net.Uri.parse(vf.toURI().toString()));view.seekTo(40);view.start();check(!view.isPlaying(),"VideoView defers start until surface");view.createSurface();check(vd.await(3,TimeUnit.SECONDS),"VideoView plays to completion");view.destroySurface();view.stopPlayback();vf.delete();
check(megler.Audio.live()==0,"no final resource leaks");System.out.println(passed+" checks passed");
}
}'''
}
paths = []
for name, source in files.items():
    p = out/'src'/name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(source)
    paths.append(p)
run(jdk/'bin/javac.exe', '-encoding', 'UTF-8', '-cp', out/'framework', '-d', out/'mocks', *paths)
run(jdk/'bin/java.exe', '-cp', str(out/'mocks')+os.pathsep+str(out/'framework'), 'TestPlayer')
