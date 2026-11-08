// Overlay of core/framework/gen-src/android/view/WindowInsetsAnimation.java (tools/build-framework.sh): Android's, where the
// generated stub kept nothing its constructors were given -- an animation of no type and no length, bounds of null, every
// callback's dispatch mode 0 -- and nothing ever made one. megler.ViewRoot now runs the keyboard's coming and going as one
// (an animation of WindowInsets.Type.ime()), through the views' callbacks (View.setWindowInsetsAnimationCallback), as
// Android's InsetsController does: React Native's keyboard controller (react-native-keyboard-controller: Discord's login and
// registration screens) moves what is above the keyboard only as such an animation tells it, and AndroidX's
// WindowInsetsAnimationCompat on Android 11 and later reads its bounds and type from these objects.
package android.view;

import android.graphics.Insets;
import android.view.animation.Interpolator;
import java.util.List;

public final class WindowInsetsAnimation {
    private final int mTypeMask;
    private final Interpolator mInterpolator;
    private final long mDurationMillis;
    private float mFraction, mAlpha = 1f;

    public WindowInsetsAnimation(int typeMask, Interpolator interpolator, long durationMillis) {
        mTypeMask = typeMask;
        mInterpolator = interpolator;
        mDurationMillis = durationMillis;
    }
    WindowInsetsAnimation() { this(0, null, 0); }
    public int getTypeMask() { return mTypeMask; }
    public float getFraction() { return mFraction; }
    public void setFraction(float fraction) { mFraction = fraction; }
    public float getInterpolatedFraction() { return mInterpolator != null ? mInterpolator.getInterpolation(mFraction) : mFraction; }
    public Interpolator getInterpolator() { return mInterpolator; }
    public long getDurationMillis() { return mDurationMillis; }
    public float getAlpha() { return mAlpha; }
    public void setAlpha(float alpha) { mAlpha = alpha; }
    @Override public String toString() { return "WindowInsetsAnimation{typeMask=" + mTypeMask + " fraction=" + mFraction + " duration=" + mDurationMillis + "}"; }

    public static final class Bounds {
        private final Insets mLowerBound, mUpperBound;
        public Bounds(Insets lowerBound, Insets upperBound) {
            mLowerBound = lowerBound != null ? lowerBound : Insets.NONE;
            mUpperBound = upperBound != null ? upperBound : Insets.NONE;
        }
        Bounds() { this(Insets.NONE, Insets.NONE); }
        public Insets getLowerBound() { return mLowerBound; }
        public Insets getUpperBound() { return mUpperBound; }
        /* Both bounds less the insets, none below nothing, as Android's. */
        public Bounds inset(Insets insets) {
            return new Bounds(Insets.max(Insets.subtract(mLowerBound, insets), Insets.NONE), Insets.max(Insets.subtract(mUpperBound, insets), Insets.NONE));
        }
        @Override public String toString() { return "Bounds{lower=" + mLowerBound + " upper=" + mUpperBound + "}"; }
    }

    public static abstract class Callback {
        public static final int DISPATCH_MODE_STOP = 0;
        public static final int DISPATCH_MODE_CONTINUE_ON_SUBTREE = 1;
        private final int mDispatchMode;
        public Callback(int dispatchMode) { mDispatchMode = dispatchMode; }
        Callback() { this(DISPATCH_MODE_STOP); }
        public final int getDispatchMode() { return mDispatchMode; }
        public void onPrepare(WindowInsetsAnimation animation) {}
        public Bounds onStart(WindowInsetsAnimation animation, Bounds bounds) { return bounds; }
        public abstract WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> runningAnimations);
        public void onEnd(WindowInsetsAnimation animation) {}
    }
}
