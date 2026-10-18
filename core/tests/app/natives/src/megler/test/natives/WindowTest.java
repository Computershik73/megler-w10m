package megler.test.natives;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.LinearLayout;

/**
 * Native code on two SurfaceViews side by side (Windows, jni/windows.c): on the left one a render thread of the library's own
 * draws with EGL on the window ANativeWindow_fromSurface gave for its surface; the right one is drawn in software through
 * ANativeWindow_lock each time its surface changes. The renderer's progress (Windows.phase) is said in the log with where
 * the views are; the test (core/tests/hk_java_tests.c, run_natives) looks at the window's picture and taps, and each tap
 * takes the next step: after phase 1 the EGL view's surface is given a fixed size of half the view's (the same window,
 * resized: the renderer's surface follows at its next swap), after phase 2 the view is taken out of the window (its surface
 * destroyed while the renderer swaps), after phase 3 it is put back (a new surface, a new window).
 */
final class WindowTest {
    private static final String T = "NativesWindow";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final LinearLayout row;
    private final SurfaceView eglView, lockView;
    private int said, acted, fullWidth, fullHeight;
    private boolean ended;

    WindowTest(Context c) {
        row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        eglView = new SurfaceView(c);
        lockView = new SurfaceView(c);
        eglView.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) { Log.i(T, "surface-created"); }
            public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
                Log.i(T, "surface-changed " + w + "x" + ht + " " + Windows.surfaceChanged(h.getSurface(), w, ht));
            }
            public void surfaceDestroyed(SurfaceHolder h) {
                Windows.surfaceDestroyed();
                Log.i(T, "surface-destroyed");
            }
        });
        lockView.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) {}
            public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) { Log.i(T, "lock " + w + "x" + ht + " " + Windows.lockPaint(h.getSurface())); }
            public void surfaceDestroyed(SurfaceHolder h) {}
        });
        row.addView(eglView, weighted());
        row.addView(lockView, weighted());
        main.postDelayed(this::poll, 100);
    }

    private static LinearLayout.LayoutParams weighted() { return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1); }

    ViewGroup view() { return row; }

    private static String where(SurfaceView v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    private void poll() {
        if (ended) return;
        int phase = Windows.phase();
        if (phase > said) {
            said = phase;
            Log.i(T, "phase " + phase + " egl=" + (eglView.getParent() != null ? where(eglView) : "gone") + " lock=" + where(lockView));
        }
        main.postDelayed(this::poll, 100);
    }

    /** The test's tap: the step after the phase last said. */
    void tapped() {
        if (acted >= said) return;
        acted = said;
        Log.i(T, "tapped after phase " + acted);
        if (acted == 1) {
            fullWidth = eglView.getWidth();
            fullHeight = eglView.getHeight();
            eglView.getHolder().setFixedSize(fullWidth / 2, fullHeight / 2);
        } else if (acted == 2) {
            row.removeView(eglView);
        } else if (acted == 3) {
            row.addView(eglView, 0, weighted());
        }
    }

    void end() {
        ended = true;
        Windows.stop();
        Log.i(T, "stopped");
    }
}
