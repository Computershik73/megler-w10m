package megler.test.gl;

import android.app.Activity;
import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;
import javax.microedition.khronos.opengles.GL10;

/**
 * OpenGL ES from an app's Java (core/src/drivers/hk_java_gl.c). The top half of the screen is a GLSurfaceView whose renderer
 * reads the GPU's strings, clears to a blue and draws a yellow triangle with a shader, with a magenta label over its bottom
 * right corner; the bottom half a SurfaceView that a thread of the app's own clears to green with EGL10 and GL10, in a
 * configuration of 565 found by exact match as apps' choosers find one. And an EGL14 pbuffer on another thread, as an app
 * asks the GPU's name. After thirty frames drawn continuously the renderer goes to RENDERMODE_WHEN_DIRTY: none while not
 * asked, one when asked; an event queued to the GL thread; the surface given a fixed size of half the view's. Each step is a
 * line of the log; the test (core/tests/hk_java_tests.c, run_gl) reads them and looks at the picture, and pauses and resumes
 * the app.
 */
public class Main extends Activity {
    private static final String T = "GlTest";
    private final Handler main = new Handler(Looper.getMainLooper());
    private GLSurfaceView glView;
    private SurfaceView eglView;
    private TextView label;
    private EglThread eglThread;
    private volatile int frames, created;
    private volatile long firstFrameNs;
    private volatile boolean continuousSaid;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        /* The OpenGL ES version the device says it offers: what a context can be made of here. */
        android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        Log.i(T, "device gles=" + am.getDeviceConfigurationInfo().getGlEsVersion() + " req=0x" + Integer.toHexString(am.getDeviceConfigurationInfo().reqGlEsVersion));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        FrameLayout top = new FrameLayout(this);
        glView = new GLSurfaceView(this);
        glView.setEGLContextClientVersion(2);
        glView.setRenderer(new Triangle());
        top.addView(glView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        label = new TextView(this);
        label.setText("over GL");
        label.setTextColor(0xFF000000);
        label.setBackgroundColor(0xFFFF00FF);
        label.setPadding(8, 8, 8, 8);
        top.addView(label, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.RIGHT));
        root.addView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        eglView = new SurfaceView(this);
        eglView.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) { eglThread = new EglThread(h); eglThread.start(); }
            public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {}
            public void surfaceDestroyed(SurfaceHolder h) { EglThread t = eglThread; eglThread = null; if (t != null) t.finish(); }
        });
        root.addView(eglView, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        new Thread(this::pbuffer, "pbuffer").start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        glView.onResume();
        Log.i(T, "onResume");
    }

    @Override
    protected void onPause() {
        glView.onPause();
        super.onPause();
        Log.i(T, "onPause frames=" + frames);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) return;
        int[] g = new int[2], l = new int[2], e = new int[2];
        glView.getLocationOnScreen(g);
        label.getLocationOnScreen(l);
        eglView.getLocationOnScreen(e);
        Log.i(T, "layout gl=" + g[0] + "," + g[1] + "," + glView.getWidth() + "x" + glView.getHeight() + " label=" + l[0] + "," + l[1] + "," + label.getWidth() + "x"
                + label.getHeight() + " egl=" + e[0] + "," + e[1] + "," + eglView.getWidth() + "x" + eglView.getHeight());
    }

    /* ------------------------------------------------------- the renderer */

    private final class Triangle implements GLSurfaceView.Renderer {
        private int program, position, color;
        private FloatBuffer vertices;

        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            int n = ++created;
            EGL10 egl = (EGL10) EGLContext.getEGL();
            EGLContext current = egl.eglGetCurrentContext();
            EGLDisplay d = egl.eglGetCurrentDisplay();
            int[] v = new int[1];
            egl.eglGetConfigAttrib(d, config, EGL10.EGL_RED_SIZE, v);
            int red = v[0];
            egl.eglGetConfigAttrib(d, config, EGL10.EGL_DEPTH_SIZE, v);
            int depth = v[0];
            egl.eglQueryContext(d, current, 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, v);
            Log.i(T, "created " + n + " thread=" + Thread.currentThread().getName() + " current=" + (current != EGL10.EGL_NO_CONTEXT) + " version=" + v[0] + " config=" + red + "/"
                    + depth);
            String ext = GLES20.glGetString(GLES20.GL_EXTENSIONS);
            Log.i(T, "strings vendor=" + GLES20.glGetString(GLES20.GL_VENDOR) + " renderer=" + GLES20.glGetString(GLES20.GL_RENDERER) + " version="
                    + GLES20.glGetString(GLES20.GL_VERSION) + " extensions=" + (ext == null ? -1 : ext.trim().split(" ").length) + " gl10=" + gl.glGetString(GL10.GL_VENDOR));
            int vs = shader(GLES20.GL_VERTEX_SHADER, "attribute vec4 aPosition;\nvoid main() { gl_Position = aPosition; }\n");
            int fs = shader(GLES20.GL_FRAGMENT_SHADER, "precision mediump float;\nuniform vec4 uColor;\nvoid main() { gl_FragColor = uColor; }\n");
            program = GLES20.glCreateProgram();
            GLES20.glAttachShader(program, vs);
            GLES20.glAttachShader(program, fs);
            GLES20.glLinkProgram(program);
            int[] linked = new int[1];
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linked, 0);
            position = GLES20.glGetAttribLocation(program, "aPosition");
            color = GLES20.glGetUniformLocation(program, "uColor");
            float[] triangle = { -0.5f, -0.5f, 0.5f, -0.5f, 0f, 0.5f };
            vertices = ByteBuffer.allocateDirect(triangle.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            vertices.put(triangle).position(0);
            Log.i(T, "program linked=" + linked[0] + " position=" + position + " color=" + color + " log=" + GLES20.glGetProgramInfoLog(program));
        }

        private int shader(int type, String source) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, source);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) Log.i(T, "shader FAILED: " + GLES20.glGetShaderInfoLog(s));
            return s;
        }

        public void onSurfaceChanged(GL10 gl, int w, int h) {
            GLES20.glViewport(0, 0, w, h);
            Log.i(T, "changed " + w + "x" + h + " on " + Thread.currentThread().getName());
        }

        public void onDrawFrame(GL10 gl) {
            GLES20.glClearColor(0f, 0.5f, 1f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT | GLES20.GL_DEPTH_BUFFER_BIT);
            GLES20.glUseProgram(program);
            GLES20.glUniform4f(color, 1f, 1f, 0f, 1f);
            GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, vertices);
            GLES20.glEnableVertexAttribArray(position);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
            int n = ++frames;
            if (n == 1) {
                firstFrameNs = System.nanoTime();
                Log.i(T, "first frame on " + Thread.currentThread().getName() + " error=" + GLES20.glGetError());
            }
            if (n == 31 && !continuousSaid) {
                continuousSaid = true;
                final long ms = (System.nanoTime() - firstFrameNs) / 1000000;
                main.post(() -> whenDirty(ms));
            }
        }
    }

    /* After thirty frames drawn continuously: none while not asked, one when asked, an event on the GL thread, a fixed size. */
    private void whenDirty(long ms) {
        Log.i(T, "continuous 30 frames in " + ms + " ms");
        glView.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        main.postDelayed(() -> {
            final int a = frames;
            main.postDelayed(() -> {
                final int b = frames;
                Log.i(T, "when-dirty idle frames=" + (b - a));
                glView.requestRender();
                main.postDelayed(() -> {
                    Log.i(T, "request-render frames=" + (frames - b));
                    glView.queueEvent(() -> {
                        EGL10 egl = (EGL10) EGLContext.getEGL();
                        Log.i(T, "event on " + Thread.currentThread().getName() + " current=" + (egl.eglGetCurrentContext() != EGL10.EGL_NO_CONTEXT) + " vendor="
                                + GLES20.glGetString(GLES20.GL_VENDOR));
                        main.post(this::fixedSize);
                    });
                }, 1000);
            }, 1000);
        }, 500);
    }

    private void fixedSize() {
        int w = glView.getWidth() / 2, h = glView.getHeight() / 2;
        glView.getHolder().setFixedSize(w, h);
        Log.i(T, "fixed size asked " + w + "x" + h);
        main.postDelayed(() -> Log.i(T, "ready frames=" + frames), 1500);
    }

    /* ------------------------------------------------ EGL14 on a pbuffer */

    /** As an app asks the GPU's name (Device Info's GPU page): a context and a 16 x 8 pbuffer on a thread of its own. */
    private void pbuffer() {
        try {
            android.opengl.EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] major = new int[1], minor = new int[1];
            EGL14.eglInitialize(d, major, 0, minor, 0);
            int[] attribs = { EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_NONE };
            android.opengl.EGLConfig[] configs = new android.opengl.EGLConfig[1];
            int[] n = new int[1];
            EGL14.eglChooseConfig(d, attribs, 0, configs, 0, 1, n, 0);
            android.opengl.EGLContext ctx = EGL14.eglCreateContext(d, configs[0], EGL14.EGL_NO_CONTEXT, new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
            android.opengl.EGLSurface s = EGL14.eglCreatePbufferSurface(d, configs[0], new int[] { EGL14.EGL_WIDTH, 16, EGL14.EGL_HEIGHT, 8, EGL14.EGL_NONE }, 0);
            boolean made = EGL14.eglMakeCurrent(d, s, s, ctx);
            String vendor = GLES20.glGetString(GLES20.GL_VENDOR), renderer = GLES20.glGetString(GLES20.GL_RENDERER), version = GLES20.glGetString(GLES20.GL_VERSION);
            String ext = GLES20.glGetString(GLES20.GL_EXTENSIONS);
            int[] max = new int[2];
            GLES20.glGetIntegerv(GLES20.GL_MAX_TEXTURE_SIZE, max, 1);
            GLES20.glClearColor(1f, 0f, 0.5f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            ByteBuffer px = ByteBuffer.allocateDirect(16 * 8 * 4).order(ByteOrder.nativeOrder());
            GLES20.glReadPixels(0, 0, 16, 8, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
            int r = px.get(0) & 0xFF, g = px.get(1) & 0xFF, b = px.get(2) & 0xFF;
            int[] width = new int[1], ver = new int[1];
            EGL14.eglQuerySurface(d, s, EGL14.EGL_WIDTH, width, 0);
            EGL14.eglQueryContext(d, ctx, EGL14.EGL_CONTEXT_CLIENT_VERSION, ver, 0);
            boolean same = EGL14.eglGetCurrentContext().equals(ctx);
            int error = GLES20.glGetError();
            EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            boolean released = EGL14.eglGetCurrentContext().equals(EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(d, s);
            EGL14.eglDestroyContext(d, ctx);
            Log.i(T, "egl14 pbuffer made=" + made + " egl=" + major[0] + "." + minor[0] + " pixel=" + r + "," + g + "," + b + " width=" + width[0] + " version=" + ver[0] + " current="
                    + same + " released=" + released + " max-texture=" + max[1] + " error=" + error);
            Log.i(T, "egl14 strings vendor=" + vendor + " renderer=" + renderer + " version=" + version + " extensions=" + (ext == null ? -1 : ext.trim().split(" ").length));
        } catch (Throwable t) {
            Log.i(T, "egl14 FAILED: " + t);
        }
    }

    /* ------------------------------------- EGL10 and GL10 on a SurfaceView */

    private static int attrib(EGL10 egl, EGLDisplay d, EGLConfig c, int name) {
        int[] v = new int[1];
        egl.eglGetConfigAttrib(d, c, name, v);
        return v[0];
    }

    /** What an older app does with a SurfaceView of its own: EGL10, a configuration found by exact match, GL10, five swaps. */
    private static final class EglThread extends Thread {
        private final SurfaceHolder holder;
        private volatile boolean stop;

        EglThread(SurfaceHolder h) {
            super("egl10");
            holder = h;
        }

        void finish() {
            stop = true;
            try {
                join(5000);
            } catch (InterruptedException e) {
                // on
            }
        }

        @Override
        public void run() {
            EGL10 egl = (EGL10) EGLContext.getEGL();
            EGLDisplay d = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
            int[] version = new int[2];
            egl.eglInitialize(d, version);
            int[] want = { EGL10.EGL_RED_SIZE, 5, EGL10.EGL_GREEN_SIZE, 6, EGL10.EGL_BLUE_SIZE, 5, EGL10.EGL_DEPTH_SIZE, 16, EGL10.EGL_NONE };
            int[] n = new int[1];
            egl.eglChooseConfig(d, want, null, 0, n);
            EGLConfig[] configs = new EGLConfig[n[0]];
            egl.eglChooseConfig(d, want, configs, n[0], n);
            EGLConfig chosen = null;
            for (EGLConfig c : configs) {
                if (attrib(egl, d, c, EGL10.EGL_RED_SIZE) == 5 && attrib(egl, d, c, EGL10.EGL_GREEN_SIZE) == 6 && attrib(egl, d, c, EGL10.EGL_BLUE_SIZE) == 5
                        && attrib(egl, d, c, EGL10.EGL_ALPHA_SIZE) == 0) {
                    chosen = c;
                    break;
                }
            }
            EGLContext ctx = egl.eglCreateContext(d, chosen, EGL10.EGL_NO_CONTEXT, null);
            EGLSurface s = egl.eglCreateWindowSurface(d, chosen, holder, null);
            boolean made = egl.eglMakeCurrent(d, s, s, ctx);
            GL10 gl = (GL10) ctx.getGL();
            int[] w = new int[1], h = new int[1];
            egl.eglQuerySurface(d, s, EGL10.EGL_WIDTH, w);
            egl.eglQuerySurface(d, s, EGL10.EGL_HEIGHT, h);
            Log.i(T, "egl10 configs=" + n[0] + " exact565=" + (chosen != null) + " made=" + made + " surface=" + w[0] + "x" + h[0] + " egl=" + version[0] + "." + version[1]);
            boolean swapped = true;
            for (int i = 0; i < 5 && !stop; i++) {
                gl.glClearColor(0f, 1f, 0f, 1f);
                gl.glClear(GL10.GL_COLOR_BUFFER_BIT);
                swapped &= egl.eglSwapBuffers(d, s);
            }
            Log.i(T, "egl10 swapped 5 ok=" + swapped + " error=" + egl.eglGetError());
            while (!stop) {
                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    break;
                }
            }
            egl.eglMakeCurrent(d, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
            egl.eglDestroySurface(d, s);
            egl.eglDestroyContext(d, ctx);
            Log.i(T, "egl10 ended");
        }
    }
}
