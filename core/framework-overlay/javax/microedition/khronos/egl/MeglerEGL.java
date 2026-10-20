// Overlay of core/framework/src/javax/microedition/khronos/egl/MeglerEGL.java (tools/build-framework.sh).
// The same EGL10/EGL11 over megler.EGLNative (core/src/drivers/hk_java_gl.c). What differs:
//  - On a GLSurfaceView's GL thread the context and the surface are the thread's real ones (the GL thread tells this class,
//    meglerRenderThread), not stand-ins with no handle: a context made on another thread can share with it
//    (eglGetCurrentContext there as the share), and eglQuerySurface gives the surface's size. The app still cannot make
//    another surface current there or destroy the thread's own, and its eglSwapBuffers of the thread's surface does
//    nothing: the thread swaps after onDrawFrame.
//  - A window surface on a SurfaceView is made with its view (EGLNative.createWindowSurface), so that its frames are shown
//    where the view is; the view itself says where that is each time it is drawn.
//  - Several configurations, not one: 8888, 888 and 565, each with depth 24 and stencil 8, with depth 16, and with neither.
//    eglChooseConfig gives those that have at least what was asked; a chooser that wants an exact match (Android's own
//    GLSurfaceView's, libGDX's) finds one. All of them are the same configuration underneath, 8888 with depth 24 and
//    stencil 8.
//  - eglSwapBuffers of no surface, or of one that was destroyed, is EGL_BAD_SURFACE, as on Android (it said true).
package javax.microedition.khronos.egl;

/**
 * EGL10/EGL11. On Megler's GL thread (where GLSurfaceView's renderer runs) it answers for the context that thread made. On any
 * other thread it is real: contexts and window surfaces from megler.EGLNative, so an app that drives EGL itself (a GLSurfaceView copied
 * into the app, an engine's own GL thread) draws, and each swap shows its frame where its SurfaceView is.
 */
final class MeglerEGL implements EGL11 {
    static final class Display extends EGLDisplay {}
    static final class Config extends EGLConfig {
        final int id, r, g, b, a, depth, stencil;
        Config(int id, int r, int g, int b, int a, int depth, int stencil) { this.id = id; this.r = r; this.g = g; this.b = b; this.a = a; this.depth = depth; this.stencil = stencil; }
    }
    static final class Surface extends EGLSurface {
        long handle; int w, h; android.view.SurfaceView view; boolean render;
        android.graphics.SurfaceTexture texture;           /* a TextureView's: each swap reads the frame back into it */
        java.nio.ByteBuffer pixels; android.graphics.Bitmap frame;
    }
    static final class Context extends EGLContext {
        long handle; int version; boolean render;
        public javax.microedition.khronos.opengles.GL getGL() { return android.opengl.GLSurfaceView.meglerGL(); }
    }
    static final Display DISPLAY = new Display();
    static final Config[] CONFIGS = {
        new Config(1, 8, 8, 8, 8, 24, 8), new Config(2, 8, 8, 8, 0, 24, 8), new Config(3, 5, 6, 5, 0, 24, 8),
        new Config(4, 8, 8, 8, 8, 16, 0), new Config(5, 8, 8, 8, 0, 16, 0), new Config(6, 5, 6, 5, 0, 16, 0),
        new Config(7, 8, 8, 8, 8, 0, 0), new Config(8, 8, 8, 8, 0, 0, 0), new Config(9, 5, 6, 5, 0, 0, 0),
    };
    static final Config CONFIG = CONFIGS[0];
    private static final ThreadLocal<Object[]> sCurrent = new ThreadLocal<>();     /* { context, draw surface } */
    private static final ThreadLocal<Integer> sError = new ThreadLocal<>();
    private static boolean render() { return megler.EGLNative.isRenderThread(); }
    private static boolean err(int e) { sError.set(e); return false; }

    /** @hide The GL thread (hk_java_gl.c): its context and surface now (0: none), for what EGL answers on it. */
    static void meglerRenderThread(long ctx, long surf, int w, int h, int version, android.view.SurfaceView view) {
        if (ctx == 0) { sCurrent.set(null); return; }
        Object[] cur = sCurrent.get();
        Context c = cur != null && cur[0] instanceof Context && ((Context) cur[0]).handle == ctx ? (Context) cur[0] : new Context();
        c.handle = ctx; c.version = version; c.render = true;
        Surface s = null;
        if (surf != 0) { s = cur != null && cur[1] instanceof Surface && ((Surface) cur[1]).handle == surf ? (Surface) cur[1] : new Surface(); s.handle = surf; s.w = w; s.h = h; s.view = view; s.render = true; }
        sCurrent.set(new Object[] { c, s != null ? s : EGL_NO_SURFACE });
    }

    private static int attrib(Config c, int attr) {
        switch (attr) {
        case EGL_RED_SIZE: return c.r; case EGL_GREEN_SIZE: return c.g; case EGL_BLUE_SIZE: return c.b; case EGL_ALPHA_SIZE: return c.a;
        case EGL_DEPTH_SIZE: return c.depth; case EGL_STENCIL_SIZE: return c.stencil; case EGL_BUFFER_SIZE: return c.r + c.g + c.b + c.a;
        case EGL_RENDERABLE_TYPE: case 0x3042 /* CONFORMANT */: return 1 | 4 | 64;
        case EGL_SURFACE_TYPE: return EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
        case EGL_CONFIG_ID: return c.id;
        case EGL_NATIVE_VISUAL_ID: return c.r == 5 ? 4 /* RGB_565 */ : c.a == 0 ? 2 /* RGBX_8888 */ : 1 /* RGBA_8888 */;
        case EGL_CONFIG_CAVEAT: case EGL_TRANSPARENT_TYPE: return EGL_NONE;
        case EGL_COLOR_BUFFER_TYPE: return EGL_RGB_BUFFER;
        case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: return 16384;
        case EGL_MAX_PBUFFER_PIXELS: return 16384 * 16384;
        case 0x303C /* MAX_SWAP_INTERVAL */: return 1;
        case EGL_LEVEL: case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: case 0x303B /* MIN_SWAP_INTERVAL */: return 0;
        default: return 0;
        }
    }
    /* Whether a configuration has what one attribute of eglChooseConfig asks: at least the size, every bit of a mask, the exact id. */
    private static boolean matches(Config c, int attr, int want) {
        if (want == EGL_DONT_CARE) return true;
        switch (attr) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_BUFFER_SIZE:
        case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
            return attrib(c, attr) >= want;
        case EGL_RENDERABLE_TYPE: case EGL_SURFACE_TYPE: case 0x3042:
            return (attrib(c, attr) & want) == want;
        case EGL_CONFIG_ID: case EGL_LEVEL: return attrib(c, attr) == want;
        case EGL_CONFIG_CAVEAT: return want == EGL_NONE;
        default: return true;
        }
    }
    /* Those that have what is asked, in EGL's order: more bits of the colours asked for, then a smaller buffer, less depth, less stencil. */
    public boolean eglChooseConfig(EGLDisplay d, int[] attribs, EGLConfig[] configs, int size, int[] num) {
        java.util.ArrayList<Config> found = new java.util.ArrayList<>();
        boolean[] asked = new boolean[4];
        for (Config c : CONFIGS) {
            boolean ok = true;
            if (attribs != null) for (int i = 0; i + 1 < attribs.length && attribs[i] != EGL_NONE && ok; i += 2) ok = matches(c, attribs[i], attribs[i + 1]);
            if (ok) found.add(c);
        }
        if (attribs != null) for (int i = 0; i + 1 < attribs.length && attribs[i] != EGL_NONE; i += 2) {
            int k = attribs[i] == EGL_RED_SIZE ? 0 : attribs[i] == EGL_GREEN_SIZE ? 1 : attribs[i] == EGL_BLUE_SIZE ? 2 : attribs[i] == EGL_ALPHA_SIZE ? 3 : -1;
            if (k >= 0 && attribs[i + 1] != 0 && attribs[i + 1] != EGL_DONT_CARE) asked[k] = true;
        }
        java.util.Collections.sort(found, (x, y) -> {
            int bx = (asked[0] ? x.r : 0) + (asked[1] ? x.g : 0) + (asked[2] ? x.b : 0) + (asked[3] ? x.a : 0);
            int by = (asked[0] ? y.r : 0) + (asked[1] ? y.g : 0) + (asked[2] ? y.b : 0) + (asked[3] ? y.a : 0);
            if (bx != by) return by - bx;
            int sx = x.r + x.g + x.b + x.a, sy = y.r + y.g + y.b + y.a;
            if (sx != sy) return sx - sy;
            if (x.depth != y.depth) return x.depth - y.depth;
            return x.stencil - y.stencil;
        });
        int n = found.size();
        if (configs != null) for (int i = 0; i < n && i < size && i < configs.length; i++) configs[i] = found.get(i);
        if (num != null && num.length > 0) num[0] = configs != null ? Math.min(n, size) : n;
        return true;
    }
    public boolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, int attr, int[] value) {
        if (!(c instanceof Config)) return err(EGL_BAD_CONFIG);
        value[0] = attrib((Config) c, attr);
        return true;
    }
    public boolean eglGetConfigs(EGLDisplay d, EGLConfig[] configs, int size, int[] num) { return eglChooseConfig(d, null, configs, size, num); }
    public EGLDisplay eglGetDisplay(Object n) { return DISPLAY; }
    public boolean eglInitialize(EGLDisplay d, int[] version) { if (version != null && version.length >= 2) { version[0] = 1; version[1] = 4; } return true; }
    public boolean eglTerminate(EGLDisplay d) { return true; }
    public EGLContext eglGetCurrentContext() { Object[] c = sCurrent.get(); return c != null && c[0] != null ? (EGLContext) c[0] : EGL_NO_CONTEXT; }
    public EGLDisplay eglGetCurrentDisplay() { Object[] c = sCurrent.get(); return c != null && c[0] != null ? DISPLAY : EGL_NO_DISPLAY; }
    public EGLSurface eglGetCurrentSurface(int which) { Object[] c = sCurrent.get(); return c != null && c[1] != null ? (EGLSurface) c[1] : EGL_NO_SURFACE; }
    static volatile int sRequestedVersion = 1;
    public EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, int[] attribs) {
        int version = 1;
        if (attribs != null) for (int i = 0; i + 1 < attribs.length && attribs[i] != EGL_NONE; i += 2) if (attribs[i] == 0x3098) version = attribs[i + 1];
        sRequestedVersion = version;                 /* what GLSurfaceView asks the app's EGLContextFactory for, on any thread */
        if (render() && eglGetCurrentContext() instanceof Context) return eglGetCurrentContext();
        long h = megler.EGLNative.createContext(version, share instanceof Context ? ((Context) share).handle : 0);
        if (h == 0) { err(EGL_BAD_ALLOC); return EGL_NO_CONTEXT; }
        Context ctx = new Context(); ctx.handle = h; ctx.version = version;
        return ctx;
    }
    public boolean eglDestroyContext(EGLDisplay d, EGLContext c) {
        if (c instanceof Context && !((Context) c).render && ((Context) c).handle != 0) { megler.EGLNative.destroyContext(((Context) c).handle); ((Context) c).handle = 0; }
        return true;
    }
    private static android.view.SurfaceView viewOf(Object w) {
        if (w instanceof android.view.SurfaceView) return (android.view.SurfaceView) w;
        if (w instanceof android.view.SurfaceHolder) { android.view.Surface s = ((android.view.SurfaceHolder) w).getSurface(); return s != null ? s.meglerView : null; }
        if (w instanceof android.view.Surface) return ((android.view.Surface) w).meglerView;
        return null;
    }
    private static android.graphics.SurfaceTexture textureOf(Object w) {
        if (w instanceof android.graphics.SurfaceTexture) return (android.graphics.SurfaceTexture) w;
        if (w instanceof android.view.Surface) return ((android.view.Surface) w).meglerTexture;
        return null;
    }
    public EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, Object w, int[] attribs) {
        android.graphics.SurfaceTexture tex = textureOf(w);
        if (tex != null) {
            int[] sz = tex.meglerSize();
            if ((sz[0] <= 0 || sz[1] <= 0) && tex.meglerView != null) sz = new int[] { tex.meglerView.getWidth(), tex.meglerView.getHeight() };
            if (sz[0] <= 0 || sz[1] <= 0) sz = new int[] { megler.Native.screenWidth(), megler.Native.screenHeight() };
            long h = megler.EGLNative.createSurface(sz[0], sz[1]);
            if (h == 0) { err(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
            Surface s = new Surface(); s.handle = h; s.w = sz[0]; s.h = sz[1]; s.texture = tex;
            return s;
        }
        if (render() && eglGetCurrentSurface(EGL_DRAW) instanceof Surface) return eglGetCurrentSurface(EGL_DRAW);
        android.view.SurfaceView v = viewOf(w);
        int[] size = v != null ? v.meglerSurfaceSize() : new int[] { megler.Native.screenWidth(), megler.Native.screenHeight() };
        long h = v != null ? megler.EGLNative.createWindowSurface(v, size[0], size[1]) : megler.EGLNative.createSurface(size[0], size[1]);
        if (h == 0) { err(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
        Surface s = new Surface(); s.handle = h; s.w = size[0]; s.h = size[1]; s.view = v;
        if (v != null) v.meglerSetGL(true);
        return s;
    }
    public EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, int[] attribs) {
        int w = 1, h = 1;
        if (attribs != null) for (int i = 0; i + 1 < attribs.length && attribs[i] != EGL_NONE; i += 2) { if (attribs[i] == EGL_WIDTH) w = attribs[i + 1]; if (attribs[i] == EGL_HEIGHT) h = attribs[i + 1]; }
        long hd = megler.EGLNative.createSurface(w, h);
        if (hd == 0) { err(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
        Surface s = new Surface(); s.handle = hd; s.w = w; s.h = h;
        return s;
    }
    public boolean eglDestroySurface(EGLDisplay d, EGLSurface s) {
        if (s instanceof Surface && !((Surface) s).render && ((Surface) s).handle != 0) { megler.EGLNative.destroySurface(((Surface) s).handle); ((Surface) s).handle = 0; }
        return true;
    }
    public boolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) {
        /* The GL thread's own context stays its: the renderer runs in it, and the thread swaps it. */
        if (render()) return true;
        long ch = c instanceof Context ? ((Context) c).handle : 0, sh = draw instanceof Surface ? ((Surface) draw).handle : 0;
        if (c != null && c != EGL_NO_CONTEXT && ch == 0) return err(EGL_BAD_CONTEXT);
        boolean ok = megler.EGLNative.makeCurrent(sh, ch);
        if (ok) sCurrent.set(ch == 0 ? null : new Object[] { c, draw != null ? draw : EGL_NO_SURFACE });
        else err(EGL_BAD_MATCH);
        return ok;
    }
    public boolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
        if (s instanceof Surface && ((Surface) s).texture != null && ((Surface) s).handle != 0) return readBack((Surface) s);
        if (!(s instanceof Surface) || ((Surface) s).handle == 0) return err(EGL_BAD_SURFACE);
        Surface su = (Surface) s;
        if (su.render) return true;
        Object[] cur = sCurrent.get();
        long ch = cur != null && cur[0] instanceof Context ? ((Context) cur[0]).handle : 0;
        int x = 0, y = 0, w = 0, h = 0;
        if (su.view != null) {
            int[] loc = new int[2];
            try { su.view.getLocationOnScreen(loc); } catch (RuntimeException e) {}
            x = loc[0]; y = loc[1]; w = su.view.getWidth(); h = su.view.getHeight();
            if (w <= 0 || h <= 0) { w = su.w; h = su.h; }
        }
        return megler.EGLNative.swap(su.handle, ch, x, y, w, h);
    }
    public int eglGetError() { Integer e = sError.get(); sError.set(EGL_SUCCESS); return e == null ? EGL_SUCCESS : e; }
    public String eglQueryString(EGLDisplay d, int name) { return name == EGL_VENDOR ? "Megler" : name == EGL_VERSION ? "1.4 Megler" : name == EGL_EXTENSIONS ? "EGL_KHR_surfaceless_context EGL_KHR_create_context" : name == 0x308D /* CLIENT_APIS */ ? "OpenGL_ES" : ""; }
    public boolean eglQuerySurface(EGLDisplay d, EGLSurface s, int attr, int[] value) {
        int w = megler.Native.screenWidth(), h = megler.Native.screenHeight();
        if (s instanceof Surface && ((Surface) s).handle != 0) { w = ((Surface) s).w; h = ((Surface) s).h; }
        value[0] = attr == EGL_WIDTH ? w : attr == EGL_HEIGHT ? h : attr == EGL_CONFIG_ID ? CONFIG.id : attr == 0x3086 /* RENDER_BUFFER */ ? 0x3084 /* BACK_BUFFER */ : 0;
        return true;
    }
    public boolean eglQueryContext(EGLDisplay d, EGLContext c, int attr, int[] value) {
        value[0] = attr == 0x3098 && c instanceof Context ? Math.max(1, ((Context) c).version) : attr == EGL_CONFIG_ID ? CONFIG.id : attr == 0x3086 ? 0x3084 : 0;
        return true;
    }
    public boolean eglReleaseThread() { if (!render()) { megler.EGLNative.makeCurrent(0, 0); sCurrent.set(null); } return true; }
    public boolean eglWaitGL() { return true; }
    public boolean eglWaitNative(int engine, Object bindTarget) { return true; }
    public boolean eglCopyBuffers(EGLDisplay d, EGLSurface s, Object target) { return true; }
    public EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, Object pixmap, int[] attribs) { return EGL_NO_SURFACE; }
    /* a TextureView's frame: read back while its context is current, flipped (GL rows run bottom-up), and posted to the texture */
    private static boolean readBack(Surface su) {
        int w = su.w, h = su.h;
        if (su.pixels == null || su.pixels.capacity() != w * h * 4) su.pixels = java.nio.ByteBuffer.allocateDirect(w * h * 4).order(java.nio.ByteOrder.nativeOrder());
        su.pixels.clear();
        android.opengl.GLES20.glReadPixels(0, 0, w, h, android.opengl.GLES20.GL_RGBA, android.opengl.GLES20.GL_UNSIGNED_BYTE, su.pixels);
        su.pixels.rewind();
        if (su.frame == null || su.frame.getWidth() != w || su.frame.getHeight() != h) su.frame = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
        su.frame.copyPixelsFromBuffer(su.pixels);
        android.graphics.Canvas c = su.texture.meglerLock(null);
        if (c == null) return true;
        try {
            c.save(); c.scale(1, -1, c.getWidth() / 2f, c.getHeight() / 2f);
            c.drawBitmap(su.frame, null, new android.graphics.Rect(0, 0, c.getWidth(), c.getHeight()), null);
            c.restore();
        } finally { su.texture.meglerPost(c); }
        return true;
    }
}
