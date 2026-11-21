package megler.test.gmem;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Random;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Mesa's freedreno in GMEM tiles on the phone, without end (drivers/mesa-arm32/src/ISSUES.md, 1: dEQP's color_clear hung the
 * GPU in GMEM, in its draw-based clears). A GLSurfaceView the whole window, RGBA 8888 without depth, drawn in three phases of
 * 30 s in turn:
 *
 *   clears  six passes a frame of twelve clears each, scissored to a random rectangle and masked to random channels (what
 *           Mesa draws as a quad, as dEQP's draw-based clears), each pass read back by one pixel (a submit and a wait);
 *   draws   sixty small triangles a frame, some scissored, masked or blended, and the swap alone;
 *   mixed   three passes a frame of eight such clears and twenty triangles, each read back.
 *
 * The same random numbers every run. Once a second the whole surface is cleared to a known color and one pixel of it read
 * back, which must be that color. Every 5 s a line "gmem: ..." says how many frames and passes went by, the longest of each
 * and the read backs right and wrong; a pass or frame that waited more than a second on the GPU is a STALL line once it
 * returns, and a watchdog says while the GL thread is still waiting (a hung GPU: the kernel's driver ends the packet after
 * some seconds, and the app may not come back from that). Nothing here knows which OpenGL ES it draws with: Megler's
 * per-app Graphics and flags decide (ui-gles-megler.test.gmem-mesa, mesa-gmem). Its frames' first command buffer stays
 * within one buffer of Mesa's, so it did not hang the GPU even before the D3DKMT core submitted a pass whole
 * (drivers/mesa-arm32/src/ISSUES.md, 1): GTA: Vice City did.
 */
public class Main extends Activity {
    static final String T = "GmemCheck";
    static final long PHASE_NS = 30000000000L;
    static final String[] PHASES = { "clears", "draws", "mixed" };

    private GLSurfaceView view;
    /* Where the GL thread is, for the watchdog: what it does now and since when. */
    volatile String step = "starting";
    volatile long stepSinceNs = System.nanoTime();
    volatile long frame;
    volatile int phase;
    private Thread watchdog;
    private volatile boolean running;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setEGLConfigChooser(8, 8, 8, 8, 0, 0);
        view.setRenderer(new Checks());
        view.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        setContentView(view);
        Log.i(T, "gmem: started");
    }

    @Override
    protected void onResume() {
        super.onResume();
        view.onResume();
        running = true;
        watchdog = new Thread(new Runnable() {
            public void run() { watch(); }
        }, "gmem-watchdog");
        watchdog.start();
    }

    @Override
    protected void onPause() {
        running = false;
        if (watchdog != null) watchdog.interrupt();
        view.onPause();
        super.onPause();
    }

    /* Says that the GL thread has been in one step for more than two seconds, and again every ten while it stays there. */
    void watch() {
        long saidSince = -1, saidAt = 0;
        while (running) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                return;
            }
            long since = stepSinceNs, now = System.nanoTime();
            long ms = (now - since) / 1000000;
            if (ms < 2000 || (since == saidSince && now - saidAt < 10000000000L)) continue;
            saidSince = since;
            saidAt = now;
            Log.w(T, "gmem: waiting " + ms + " ms in " + step + " (frame " + frame + ", phase " + PHASES[phase] + ")");
        }
    }

    final class Checks implements GLSurfaceView.Renderer {
        private final Random random = new Random(20261121);
        private final ByteBuffer pixel = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
        private FloatBuffer triangle;
        private int program, aPos, uOffset, uColor;
        private int width = 1, height = 1;
        private long phaseStartNs, periodStartNs;
        private long periodFrames, periodPasses, totalFrames, totalPasses, stalls;
        private double longestPassMs, longestFrameMs;
        private int errorsSaid;
        private long checksRight, checksWrong;
        private long lastCheckNs;

        public void onSurfaceCreated(GL10 unused, EGLConfig config) {
            Log.i(T, "gmem: OpenGL ES " + GLES20.glGetString(GLES20.GL_VERSION) + ", " + GLES20.glGetString(GLES20.GL_RENDERER));
            program = link("attribute vec2 p; uniform vec4 o; void main() { gl_Position = vec4(p * o.zw + o.xy, 0.0, 1.0); }",
                           "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }");
            aPos = GLES20.glGetAttribLocation(program, "p");
            uOffset = GLES20.glGetUniformLocation(program, "o");
            uColor = GLES20.glGetUniformLocation(program, "c");
            float[] v = { -1f, -1f, 1f, -1f, 0f, 1f };
            triangle = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            triangle.put(v).position(0);
            phaseStartNs = periodStartNs = System.nanoTime();
        }

        public void onSurfaceChanged(GL10 unused, int w, int h) {
            width = Math.max(1, w);
            height = Math.max(1, h);
            GLES20.glViewport(0, 0, width, height);
            Log.i(T, "gmem: the surface is " + width + "x" + height);
        }

        public void onDrawFrame(GL10 unused) {
            long t0 = System.nanoTime();
            if (t0 - phaseStartNs >= PHASE_NS) {
                phase = (phase + 1) % PHASES.length;
                phaseStartNs = t0;
                Log.i(T, "gmem: phase " + PHASES[phase] + " from frame " + totalFrames);
            }
            frame = totalFrames;
            enter("the frame's first pass");
            if (t0 - lastCheckNs >= 1000000000L) {
                lastCheckNs = t0;
                check();
            }
            GLES20.glClearColor(0f, 0f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            switch (phase) {
            case 0:
                for (int pass = 0; pass < 6; pass++) {
                    clears(12);
                    readBack();
                }
                break;
            case 1:
                triangles(60);
                break;
            default:
                for (int pass = 0; pass < 3; pass++) {
                    clears(8);
                    triangles(20);
                    readBack();
                }
                break;
            }
            int error = GLES20.glGetError();
            if (error != GLES20.GL_NO_ERROR && errorsSaid++ < 20)
                Log.w(T, "gmem: OpenGL ES error 0x" + Integer.toHexString(error) + " at frame " + totalFrames);
            enter("the swap after frame " + totalFrames);
            long t1 = System.nanoTime();
            double ms = (t1 - t0) / 1e6;
            if (ms > longestFrameMs) longestFrameMs = ms;
            if (ms > 1000) {
                stalls++;
                Log.w(T, "gmem: STALL: frame " + totalFrames + " took " + (long) ms + " ms (phase " + PHASES[phase] + ")");
            }
            totalFrames++;
            periodFrames++;
            if (t1 - periodStartNs >= 5000000000L) {
                double s = (t1 - periodStartNs) / 1e9;
                Log.i(T, String.format(java.util.Locale.ROOT,
                        "gmem: %s %.1f s: %d frames (%.1f a second), %d passes read back; the longest pass %.1f ms, the longest frame "
                                + "%.1f ms; in all %d frames, %d passes, %d stalls; read back right %d times, wrong %d",
                        PHASES[phase], s, periodFrames, periodFrames / s, periodPasses, longestPassMs, longestFrameMs, totalFrames,
                        totalPasses, stalls, checksRight, checksWrong));
                periodStartNs = t1;
                periodFrames = periodPasses = 0;
                longestPassMs = longestFrameMs = 0;
            }
        }

        /*
         * Once a second: the whole surface cleared to a color of the frame's number and one pixel read back, which must be
         * that color -- a read back that maps nothing (a window surface the CPU cannot see) returns what the buffer had.
         */
        private void check() {
            int r = (int) (totalFrames * 37 % 251), g = (int) (totalFrames * 11 % 241), b = 128;
            GLES20.glClearColor(r / 255f, g / 255f, b / 255f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            pixel.position(0);
            pixel.put(0, (byte) 1).put(1, (byte) 2).put(2, (byte) 3).put(3, (byte) 4);
            enter("a check read back at frame " + totalFrames);
            GLES20.glReadPixels(width / 2, height / 2, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pixel);
            int gr = pixel.get(0) & 0xff, gg = pixel.get(1) & 0xff, gb = pixel.get(2) & 0xff, ga = pixel.get(3) & 0xff;
            boolean right = Math.abs(gr - r) <= 1 && Math.abs(gg - g) <= 1 && Math.abs(gb - b) <= 1 && ga == 255;
            if (right) {
                checksRight++;
            } else if (checksWrong++ < 5) {
                Log.w(T, String.format(java.util.Locale.ROOT, "gmem: read back wrong at frame %d: %d,%d,%d,%d where %d,%d,%d,255 was drawn",
                        totalFrames, gr, gg, gb, ga, r, g, b));
            }
        }

        /* (The step the GL thread is in, for the watchdog.) */
        private void enter(String what) {
            step = what;
            stepSinceNs = System.nanoTime();
        }

        /* Clears as dEQP's color_clear does them: a random rectangle, random channels, a random color. */
        private void clears(int n) {
            GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
            for (int i = 0; i < n; i++) {
                int x = random.nextInt(width), y = random.nextInt(height);
                int w = 1 + random.nextInt(width - x), h = 1 + random.nextInt(height - y);
                if (random.nextInt(8) == 0) {
                    x = y = 0;
                    w = width;
                    h = height;
                }
                GLES20.glScissor(x, y, w, h);
                int mask = random.nextInt(8) == 0 ? 15 : 1 + random.nextInt(15);
                GLES20.glColorMask((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
                GLES20.glClearColor(random.nextFloat(), random.nextFloat(), random.nextFloat(), random.nextFloat());
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            }
            GLES20.glColorMask(true, true, true, true);
            GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        }

        /* Small triangles of one color each, some scissored, masked or blended. */
        private void triangles(int n) {
            GLES20.glUseProgram(program);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, triangle);
            GLES20.glEnableVertexAttribArray(aPos);
            for (int i = 0; i < n; i++) {
                boolean scissor = random.nextInt(3) == 0, blend = random.nextBoolean(), mask = random.nextInt(4) == 0;
                if (scissor) {
                    GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                    int x = random.nextInt(width), y = random.nextInt(height);
                    GLES20.glScissor(x, y, 1 + random.nextInt(width - x), 1 + random.nextInt(height - y));
                }
                if (blend) {
                    GLES20.glEnable(GLES20.GL_BLEND);
                    GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA);
                }
                if (mask) GLES20.glColorMask(random.nextBoolean(), random.nextBoolean(), random.nextBoolean(), true);
                float size = 0.05f + random.nextFloat() * 0.5f;
                GLES20.glUniform4f(uOffset, random.nextFloat() * 2f - 1f, random.nextFloat() * 2f - 1f, size, size);
                GLES20.glUniform4f(uColor, random.nextFloat(), random.nextFloat(), random.nextFloat(), 0.25f + random.nextFloat() * 0.75f);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
                if (scissor) GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                if (blend) GLES20.glDisable(GLES20.GL_BLEND);
                if (mask) GLES20.glColorMask(true, true, true, true);
            }
            GLES20.glDisableVertexAttribArray(aPos);
        }

        /* One pixel read back: the batch submitted and waited for. */
        private void readBack() {
            int x = random.nextInt(width), y = random.nextInt(height);
            enter("a pass read back at frame " + totalFrames);
            long t0 = System.nanoTime();
            pixel.position(0);
            GLES20.glReadPixels(x, y, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pixel);
            double ms = (System.nanoTime() - t0) / 1e6;
            enter("a pass after frame " + totalFrames + "'s read back");
            totalPasses++;
            periodPasses++;
            if (ms > longestPassMs) longestPassMs = ms;
            if (ms > 1000) {
                stalls++;
                Log.w(T, "gmem: STALL: a pass of frame " + totalFrames + " took " + (long) ms + " ms to read back (phase "
                        + PHASES[phase] + ")");
            }
        }

        private int link(String vs, String fs) {
            int p = GLES20.glCreateProgram();
            GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
            GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
            GLES20.glLinkProgram(p);
            int[] ok = new int[1];
            GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
            if (ok[0] == 0) Log.e(T, "gmem: the program did not link: " + GLES20.glGetProgramInfoLog(p));
            return p;
        }

        private int shader(int type, String source) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, source);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) Log.e(T, "gmem: a shader did not compile: " + GLES20.glGetShaderInfoLog(s));
            return s;
        }
    }
}
