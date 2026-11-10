package megler.test.natives;

import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

/**
 * Two SurfaceViews one over the other, as HERE WeGo has its map and Flutter's view of its whole interface (Windows'
 * stack, jni/windows.c): under, an ordinary SurfaceView; over it, one ordered on top of the window (setZOrderOnTop) with a
 * transparent format, as FlutterActivity's TransparencyMode.transparent makes Flutter's; and between them, in the window,
 * two yellow boxes of the views. A render thread of the library's own draws on each surface's window without a pause, the
 * two at once: under, blue all over; over, clear, its right half red at half alpha. On Android the window shows the blue
 * and the left box through the clear left half, the red over the blue and the right box in the right half. Said in the log
 * once both have drawn thirty frames, with where the views are; the test (core/tests/mg_java_tests.c, natives_stack)
 * looks at the window's picture there.
 */
final class StackTest {
    private static final String T = "NativesStack";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final FrameLayout stack;
    private final SurfaceView under, over;
    private final View left, right;
    private boolean said, ended;

    StackTest(Context c) {
        stack = new FrameLayout(c);
        under = new SurfaceView(c);
        over = new SurfaceView(c);
        over.setZOrderOnTop(true);
        over.getHolder().setFormat(PixelFormat.TRANSPARENT);
        left = new View(c);
        right = new View(c);
        left.setBackgroundColor(0xFFFFFF00);
        right.setBackgroundColor(0xFFFFFF00);
        stack.addView(under, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        stack.addView(left, new FrameLayout.LayoutParams(0, 0));
        stack.addView(right, new FrameLayout.LayoutParams(0, 0));
        stack.addView(over, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // The boxes: 10% to 30% and 70% to 90% of the width, the middle half of the height.
        stack.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            final int w = r - l, h = b - t;
            stack.post(() -> {
                place(left, w / 10, h / 4, w / 5, h / 2);
                place(right, w * 7 / 10, h / 4, w / 5, h / 2);
            });
        });
        under.getHolder().addCallback(new Callback(0));
        over.getHolder().addCallback(new Callback(1));
        main.postDelayed(this::poll, 100);
    }

    private static void place(View v, int x, int y, int w, int h) {
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams) v.getLayoutParams();
        if (p.leftMargin == x && p.topMargin == y && p.width == w && p.height == h) return;
        p.leftMargin = x;
        p.topMargin = y;
        p.width = w;
        p.height = h;
        v.setLayoutParams(p);
    }

    private final class Callback implements SurfaceHolder.Callback {
        private final int which;
        Callback(int which) { this.which = which; }
        public void surfaceCreated(SurfaceHolder h) {}
        public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
            Log.i(T, (which == 1 ? "over" : "under") + " surface " + w + "x" + ht + " window format " + Windows.stackWindow(which, h.getSurface()));
        }
        public void surfaceDestroyed(SurfaceHolder h) {}
    }

    ViewGroup view() { return stack; }

    private static String where(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    private void poll() {
        if (ended) return;
        if (!said && Windows.stackFrames(0) >= 30 && Windows.stackFrames(1) >= 30) {
            said = true;
            Log.i(T, "drawn under=" + Windows.stackFrames(0) + " over=" + Windows.stackFrames(1) + " stack=" + where(stack) + " left=" + where(left)
                     + " right=" + where(right));
        }
        main.postDelayed(this::poll, 100);
    }

    void end() {
        ended = true;
        Windows.stackStop();
        Log.i(T, "stopped");
    }
}
