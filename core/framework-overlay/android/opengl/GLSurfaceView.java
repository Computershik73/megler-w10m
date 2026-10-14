// megler-w10m overlay of Megler/src/java-framework/src/android/opengl/GLSurfaceView.java (tools/build-framework.sh).
// The renderer runs on a GL thread of the layer's own, as in the original (megler.Native.startGL), but that thread is
// core/src/drivers/hk_java_gl.c here, and what it does each turn this class decides in meglerFrame: the queued events,
// then end, no surface, paused, draw, or nothing to draw. What differs from the original, which drew on one render thread
// that also put the views on the screen:
//  - the surface's life as Android's GLThread follows it: its size from surfaceChanged (a fixed size included), its going
//    in surfaceDestroyed, onPause and onResume, and the view leaving its window, which ends the thread (a new one is
//    started when it comes back); setPreserveEGLContextOnPause is kept and asked;
//  - every change wakes the thread (megler.Native.requestRender), which otherwise waits when it has nothing to draw;
//  - the configuration onSurfaceCreated is given is what the app's EGLConfigChooser chooses, or one of the sizes it asked
//    for, and the GL10 is what the app's GLWrapper makes of it;
//  - setRenderer twice is the IllegalStateException Android throws, queueEvent(null) its IllegalArgumentException.
package android.opengl;

import java.util.ArrayList;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;
import javax.microedition.khronos.opengles.GL10;

/** The renderer runs on Megler's GL thread (core/src/drivers/hk_java_gl.c), which owns the EGL context and a surface the view's size. */
public class GLSurfaceView extends android.view.SurfaceView implements android.view.SurfaceHolder.Callback2 {
    public static final int RENDERMODE_WHEN_DIRTY = 0, RENDERMODE_CONTINUOUSLY = 1, DEBUG_CHECK_GL_ERROR = 1, DEBUG_LOG_GL_CALLS = 2;
    public interface Renderer { void onSurfaceCreated(GL10 gl, EGLConfig config); void onSurfaceChanged(GL10 gl, int w, int h); void onDrawFrame(GL10 gl); }
    public interface EGLConfigChooser { EGLConfig chooseConfig(EGL10 egl, EGLDisplay display); }
    public interface EGLContextFactory { EGLContext createContext(EGL10 egl, EGLDisplay d, EGLConfig c); void destroyContext(EGL10 egl, EGLDisplay d, EGLContext c); }
    public interface EGLWindowSurfaceFactory { EGLSurface createWindowSurface(EGL10 egl, EGLDisplay d, EGLConfig c, Object w); void destroySurface(EGL10 egl, EGLDisplay d, EGLSurface s); }
    public interface GLWrapper { javax.microedition.khronos.opengles.GL wrap(javax.microedition.khronos.opengles.GL gl); }

    /* What meglerFrame answers, with the surface's width and height above it: as hk_java_gl.c reads it. */
    private static final int FRAME_DRAW = 1, FRAME_EXIT = 2, FRAME_NO_SURFACE = 4, FRAME_PAUSED = 8, FRAME_PRESERVE = 16;

    private Renderer renderer;
    private int version = 1, renderMode = RENDERMODE_CONTINUOUSLY, debugFlags;
    private boolean versionSet, preserve;
    private EGLContextFactory contextFactory;
    private EGLConfigChooser configChooser;
    private int[] configSizes = { 8, 8, 8, 0, 16, 0 };            /* what a GLSurfaceView asks for when told nothing: RGB 888 with depth */
    private GLWrapper wrapper;
    private volatile boolean dirty = true, started, paused, hasSurface, exit;
    private volatile int surfaceW, surfaceH, generation;
    private final ArrayList<Runnable> queue = new ArrayList<>();
    /* Which start a GL thread belongs to: one that was left behind by a detach and a quick attach ends all the same. */
    private final ThreadLocal<Integer> threadGeneration = new ThreadLocal<>();

    public GLSurfaceView(android.content.Context c) { this(c, null); }
    public GLSurfaceView(android.content.Context c, android.util.AttributeSet a) { super(c, a); getHolder().addCallback(this); }
    public void surfaceCreated(android.view.SurfaceHolder h) { hasSurface = true; start(); wake(); }
    public void surfaceChanged(android.view.SurfaceHolder h, int format, int w, int ht) { surfaceW = w; surfaceH = ht; hasSurface = true; dirty = true; wake(); }
    public void surfaceDestroyed(android.view.SurfaceHolder h) { hasSurface = false; wake(); }
    public void surfaceRedrawNeeded(android.view.SurfaceHolder h) { requestRender(); }
    public void surfaceRedrawNeededAsync(android.view.SurfaceHolder h, Runnable done) { requestRender(); done.run(); }
    public void setRenderer(Renderer r) {
        if (renderer != null) throw new IllegalStateException("setRenderer has already been called for this instance.");
        renderer = r;
        if (isAttachedToWindow()) start();
    }
    public void setEGLContextClientVersion(int v) { version = v; versionSet = true; }
    public void setEGLConfigChooser(EGLConfigChooser c) { configChooser = c; }
    public void setEGLConfigChooser(boolean depth) { configSizes = new int[] { 8, 8, 8, 0, depth ? 16 : 0, 0 }; configChooser = null; }
    public void setEGLConfigChooser(int r, int g, int b, int a, int depth, int stencil) { configSizes = new int[] { r, g, b, a, depth, stencil }; configChooser = null; }
    public void setEGLContextFactory(EGLContextFactory f) { contextFactory = f; }
    public void setEGLWindowSurfaceFactory(EGLWindowSurfaceFactory f) {}
    public void setGLWrapper(GLWrapper w) { wrapper = w; }
    public void setDebugFlags(int f) { debugFlags = f; }
    public int getDebugFlags() { return debugFlags; }
    public void setPreserveEGLContextOnPause(boolean b) { preserve = b; }
    public boolean getPreserveEGLContextOnPause() { return preserve; }
    public void setRenderMode(int m) { renderMode = m; dirty = true; wake(); }
    public int getRenderMode() { return renderMode; }
    public void requestRender() { dirty = true; wake(); }
    public void onPause() { paused = true; wake(); }
    public void onResume() { paused = false; dirty = true; wake(); }
    public void queueEvent(Runnable r) {
        if (r == null) throw new IllegalArgumentException("r must not be null");
        synchronized (queue) { queue.add(r); }
        wake();
    }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); if (renderer != null && !started) start(); }
    /* As Android's: the GL thread ends when the view leaves its window, and its context goes with it. */
    @Override protected void onDetachedFromWindow() { if (started) { exit = true; started = false; wake(); } super.onDetachedFromWindow(); }
    private static void wake() { megler.Native.requestRender(); }
    private void start() {
        if (renderer == null || started) return;
        started = true;
        generation++;
        exit = false;
        // an app with its own context factory asks for its GL ES version there (EGL_CONTEXT_CLIENT_VERSION), not here
        if (!versionSet && contextFactory != null) {
            try {
                EGL10 egl = (EGL10) EGLContext.getEGL();
                EGLDisplay d = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
                EGLConfig[] cfg = new EGLConfig[1];
                egl.eglChooseConfig(d, null, cfg, 1, new int[1]);
                EGLContext probe = contextFactory.createContext(egl, d, cfg[0]);
                version = Math.max(1, EGLContext.meglerRequestedVersion());
                // made off the render thread, that is a real context of its own: the render thread makes the one the app draws with
                if (probe != null && probe != EGL10.EGL_NO_CONTEXT) try { contextFactory.destroyContext(egl, d, probe); } catch (RuntimeException e) { }
            } catch (RuntimeException e) {
                android.util.Log.w("GLSurfaceView", "the app's EGLContextFactory threw: " + e);
                version = 2;
            }
        }
        megler.Native.startGL(this, renderer, version);
    }

    private static MeglerGL10 sGL;
    /** @hide The GL10 object EGLContext.getGL() hands out. */
    public static synchronized javax.microedition.khronos.opengles.GL meglerGL() { if (sGL == null) sGL = new MeglerGL10(); return sGL; }

    /** @hide The GL thread, once: the GL10 the renderer is handed, as the app's GLWrapper makes it. */
    public final javax.microedition.khronos.opengles.GL meglerRendererGL() { javax.microedition.khronos.opengles.GL gl = meglerGL(); return wrapper != null ? wrapper.wrap(gl) : gl; }

    /** @hide The GL thread, once a context is made: the configuration onSurfaceCreated is given. */
    public final EGLConfig meglerConfig() {
        EGL10 egl = (EGL10) EGLContext.getEGL();
        EGLDisplay d = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
        if (configChooser != null) {
            try { EGLConfig c = configChooser.chooseConfig(egl, d); if (c != null) return c; }
            catch (RuntimeException e) { android.util.Log.w("GLSurfaceView", "the app's EGLConfigChooser threw: " + e); }
        }
        int[] s = configSizes;
        int[] want = { EGL10.EGL_RED_SIZE, s[0], EGL10.EGL_GREEN_SIZE, s[1], EGL10.EGL_BLUE_SIZE, s[2], EGL10.EGL_ALPHA_SIZE, s[3], EGL10.EGL_DEPTH_SIZE, s[4], EGL10.EGL_STENCIL_SIZE, s[5], EGL10.EGL_NONE };
        EGLConfig[] found = new EGLConfig[16];
        int[] n = new int[1];
        egl.eglChooseConfig(d, want, found, found.length, n);
        // Android's chooser takes the first whose colour sizes are exactly what was asked
        for (int i = 0; i < n[0]; i++) {
            int[] v = new int[1];
            boolean exact = true;
            for (int k = 0; k < 4 && exact; k++) { egl.eglGetConfigAttrib(d, found[i], want[2 * k], v); exact = v[0] == s[k]; }
            if (exact) return found[i];
        }
        return n[0] > 0 ? found[0] : null;
    }

    /** @hide The GL thread, each turn: the queued events, then what to do and the surface's size (hk_java_gl.c reads it). */
    public final long meglerFrame() {
        Integer mine = threadGeneration.get();
        if (mine == null) threadGeneration.set(mine = generation);
        if (exit || mine != generation) return FRAME_EXIT;
        ArrayList<Runnable> run = null;
        synchronized (queue) { if (!queue.isEmpty()) { run = new ArrayList<>(queue); queue.clear(); } }
        if (run != null) for (Runnable r : run) r.run();
        int flags = preserve ? FRAME_PRESERVE : 0;
        if (exit) return FRAME_EXIT;
        if (paused) return flags | FRAME_PAUSED;
        int w = surfaceW, h = surfaceH;
        if (!hasSurface || w <= 0 || h <= 0) return flags | FRAME_NO_SURFACE;
        if (renderMode == RENDERMODE_CONTINUOUSLY || dirty) { dirty = false; flags |= FRAME_DRAW; }
        return ((long) (w & 0xFFFF) << 32) | ((long) (h & 0xFFFF) << 16) | flags;
    }
}
