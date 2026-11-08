package android.view;

import android.graphics.Insets;
import android.view.animation.Interpolator;
import java.util.List;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the typing screen calls of Android 11's inset animations. */
public final class WindowInsetsAnimation {
    public WindowInsetsAnimation(int typeMask, Interpolator interpolator, long durationMillis) {}
    public int getTypeMask() { return 0; }
    public float getFraction() { return 0; }
    public long getDurationMillis() { return 0; }

    public static final class Bounds {
        public Bounds(Insets lowerBound, Insets upperBound) {}
        public Insets getLowerBound() { return null; }
        public Insets getUpperBound() { return null; }
    }

    public static abstract class Callback {
        public static final int DISPATCH_MODE_STOP = 0;
        public static final int DISPATCH_MODE_CONTINUE_ON_SUBTREE = 1;
        public Callback(int dispatchMode) {}
        public void onPrepare(WindowInsetsAnimation animation) {}
        public Bounds onStart(WindowInsetsAnimation animation, Bounds bounds) { return bounds; }
        public abstract WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> runningAnimations);
        public void onEnd(WindowInsetsAnimation animation) {}
    }
}
