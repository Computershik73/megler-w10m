package android.view;

import android.animation.Animator;
import android.animation.TimeInterpolator;
import android.animation.ValueAnimator;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * view.animate(), as Android's: the properties asked for since the last start are animated together by a ValueAnimator of
 * their own, started by start() (even with nothing asked for) or on the next frame. A property asked for again is taken
 * from the animator that had it, there and then, and that animator is cancelled once it has none left; animators of other
 * properties run on beside the new one. The listener and the update listener are told as Android tells them: the ones this
 * animator has when an event comes, whichever animator it comes from -- so a listener taken off in onAnimationCancel does
 * not hear the onAnimationEnd that follows. withStartAction's and withEndAction's runnables belong to the animator started
 * next, and the end action is not run for an animator cancelled.
 *
 * Before, one animation ran at a time, the next start cancelled it, and it told the listeners set when it started:
 * Spotify's carousel animator (through ViewPropertyAnimatorCompat) takes its listener off when an item's fade is
 * cancelled, so as to finish the item once, and finished it twice.
 */
public class ViewPropertyAnimator {
    private static final int NONE = 0, TRANSLATION_X = 1, TRANSLATION_Y = 2, TRANSLATION_Z = 4, SCALE_X = 8, SCALE_Y = 16, ROTATION = 32,
        ROTATION_X = 64, ROTATION_Y = 128, X = 256, Y = 512, Z = 1024, ALPHA = 2048;
    private final View mView;
    private long mDuration, mStartDelay;
    private boolean mDurationSet, mStartDelaySet, mInterpolatorSet;
    private TimeInterpolator mInterpolator;
    private Animator.AnimatorListener mListener;
    private ValueAnimator.AnimatorUpdateListener mUpdateListener;
    /** The properties asked for since the last start: each {constant, from, delta}. */
    private final ArrayList<float[]> mPending = new ArrayList<>();
    private Runnable mPendingOnStart, mPendingOnEnd;
    /** The animators running, each with the properties it still animates. */
    private final HashMap<Animator, Props> mAnimators = new HashMap<>();
    private final HashMap<Animator, Runnable> mOnStart = new HashMap<>(), mOnEnd = new HashMap<>();
    private final Events mEvents = new Events();
    private final Runnable mStarter = this::startAnimation;

    ViewPropertyAnimator(View v) { mView = v; }

    public ViewPropertyAnimator setDuration(long d) {
        if (d < 0) throw new IllegalArgumentException("Animators cannot have negative duration: " + d);
        mDurationSet = true;
        mDuration = d;
        return this;
    }
    public long getDuration() { return mDurationSet ? mDuration : 300; }
    public long getStartDelay() { return mStartDelaySet ? mStartDelay : 0; }
    public ViewPropertyAnimator setStartDelay(long d) {
        if (d < 0) throw new IllegalArgumentException("Animators cannot have negative start delay: " + d);
        mStartDelaySet = true;
        mStartDelay = d;
        return this;
    }
    public ViewPropertyAnimator setInterpolator(TimeInterpolator i) { mInterpolatorSet = true; mInterpolator = i; return this; }
    public TimeInterpolator getInterpolator() { return mInterpolatorSet ? mInterpolator : new ValueAnimator().getInterpolator(); }
    public ViewPropertyAnimator setListener(Animator.AnimatorListener l) { mListener = l; return this; }
    public ViewPropertyAnimator setUpdateListener(ValueAnimator.AnimatorUpdateListener l) { mUpdateListener = l; return this; }
    public ViewPropertyAnimator withLayer() { return this; }
    public ViewPropertyAnimator withStartAction(Runnable r) { mPendingOnStart = r; return this; }
    public ViewPropertyAnimator withEndAction(Runnable r) { mPendingOnEnd = r; return this; }

    /** What was asked for since the last start, started now. */
    public void start() {
        mView.removeCallbacks(mStarter);
        startAnimation();
    }
    /** Every animator running cancelled, and what was asked for and not started yet forgotten. */
    public void cancel() {
        if (!mAnimators.isEmpty()) for (Animator a : new ArrayList<>(mAnimators.keySet())) a.cancel();
        mPending.clear();
        mPendingOnStart = null;
        mPendingOnEnd = null;
        mView.removeCallbacks(mStarter);
    }

    public ViewPropertyAnimator x(float v) { return to(X, v); } public ViewPropertyAnimator xBy(float v) { return by(X, v); }
    public ViewPropertyAnimator y(float v) { return to(Y, v); } public ViewPropertyAnimator yBy(float v) { return by(Y, v); }
    public ViewPropertyAnimator z(float v) { return to(Z, v); } public ViewPropertyAnimator zBy(float v) { return by(Z, v); }
    public ViewPropertyAnimator rotation(float v) { return to(ROTATION, v); } public ViewPropertyAnimator rotationBy(float v) { return by(ROTATION, v); }
    public ViewPropertyAnimator rotationX(float v) { return to(ROTATION_X, v); } public ViewPropertyAnimator rotationXBy(float v) { return by(ROTATION_X, v); }
    public ViewPropertyAnimator rotationY(float v) { return to(ROTATION_Y, v); } public ViewPropertyAnimator rotationYBy(float v) { return by(ROTATION_Y, v); }
    public ViewPropertyAnimator translationX(float v) { return to(TRANSLATION_X, v); } public ViewPropertyAnimator translationXBy(float v) { return by(TRANSLATION_X, v); }
    public ViewPropertyAnimator translationY(float v) { return to(TRANSLATION_Y, v); } public ViewPropertyAnimator translationYBy(float v) { return by(TRANSLATION_Y, v); }
    public ViewPropertyAnimator translationZ(float v) { return to(TRANSLATION_Z, v); } public ViewPropertyAnimator translationZBy(float v) { return by(TRANSLATION_Z, v); }
    public ViewPropertyAnimator scaleX(float v) { return to(SCALE_X, v); } public ViewPropertyAnimator scaleXBy(float v) { return by(SCALE_X, v); }
    public ViewPropertyAnimator scaleY(float v) { return to(SCALE_Y, v); } public ViewPropertyAnimator scaleYBy(float v) { return by(SCALE_Y, v); }
    public ViewPropertyAnimator alpha(float v) { return to(ALPHA, v); } public ViewPropertyAnimator alphaBy(float v) { return by(ALPHA, v); }

    private ViewPropertyAnimator to(int p, float v) { float from = get(p); return animate(p, from, v - from); }
    private ViewPropertyAnimator by(int p, float v) { return animate(p, get(p), v); }
    /** The property taken from the animator running it (cancelled if that leaves it nothing), and asked for anew. */
    private ViewPropertyAnimator animate(int p, float from, float delta) {
        if (!mAnimators.isEmpty()) {
            Animator emptied = null;
            for (java.util.Map.Entry<Animator, Props> e : mAnimators.entrySet()) {
                if (e.getValue().cancel(p) && e.getValue().mask == NONE) { emptied = e.getKey(); break; }
            }
            if (emptied != null) emptied.cancel();
        }
        mPending.add(new float[] { p, from, delta });
        mView.removeCallbacks(mStarter);
        mView.postOnAnimation(mStarter);
        return this;
    }
    private float get(int p) {
        switch (p) {
        case TRANSLATION_X: return mView.getTranslationX(); case TRANSLATION_Y: return mView.getTranslationY(); case TRANSLATION_Z: return mView.getTranslationZ();
        case SCALE_X: return mView.getScaleX(); case SCALE_Y: return mView.getScaleY(); case ROTATION: return mView.getRotation();
        case ROTATION_X: return mView.getRotationX(); case ROTATION_Y: return mView.getRotationY();
        case X: return mView.getX(); case Y: return mView.getY(); case Z: return mView.getZ(); default: return mView.getAlpha();
        }
    }
    private void set(int p, float v) {
        switch (p) {
        case TRANSLATION_X: mView.setTranslationX(v); break; case TRANSLATION_Y: mView.setTranslationY(v); break; case TRANSLATION_Z: mView.setTranslationZ(v); break;
        case SCALE_X: mView.setScaleX(v); break; case SCALE_Y: mView.setScaleY(v); break; case ROTATION: mView.setRotation(v); break;
        case ROTATION_X: mView.setRotationX(v); break; case ROTATION_Y: mView.setRotationY(v); break;
        case X: mView.setX(v); break; case Y: mView.setY(v); break; case Z: mView.setZ(v); break; default: mView.setAlpha(v);
        }
    }

    private void startAnimation() {
        mView.setHasTransientState(true);
        ValueAnimator va = ValueAnimator.ofFloat(0f, 1f);
        ArrayList<float[]> props = new ArrayList<>(mPending);
        mPending.clear();
        int mask = NONE;
        for (float[] p : props) mask |= (int) p[0];
        mAnimators.put(va, new Props(mask, props));
        if (mPendingOnStart != null) { mOnStart.put(va, mPendingOnStart); mPendingOnStart = null; }
        if (mPendingOnEnd != null) { mOnEnd.put(va, mPendingOnEnd); mPendingOnEnd = null; }
        va.addUpdateListener(mEvents);
        va.addListener(mEvents);
        if (mStartDelaySet) va.setStartDelay(mStartDelay);
        if (mDurationSet) va.setDuration(mDuration);
        if (mInterpolatorSet) va.setInterpolator(mInterpolator);
        va.start();
    }

    /** The properties one animator animates, less those a later one took. */
    private static final class Props {
        int mask;
        final ArrayList<float[]> values;
        Props(int mask, ArrayList<float[]> values) { this.mask = mask; this.values = values; }
        boolean cancel(int p) {
            if ((mask & p) == 0) return false;
            for (int i = 0; i < values.size(); i++) {
                if ((int) values.get(i)[0] == p) { values.remove(i); mask &= ~p; return true; }
            }
            return false;
        }
    }

    /** What every animator tells: passed to the listeners this animator has now, and the property values set. */
    private final class Events implements Animator.AnimatorListener, ValueAnimator.AnimatorUpdateListener {
        @Override public void onAnimationStart(Animator a) {
            Runnable r = mOnStart.remove(a);
            if (r != null) r.run();
            if (mListener != null) mListener.onAnimationStart(a);
        }
        @Override public void onAnimationCancel(Animator a) {
            if (mListener != null) mListener.onAnimationCancel(a);
            mOnEnd.remove(a);
        }
        @Override public void onAnimationRepeat(Animator a) { if (mListener != null) mListener.onAnimationRepeat(a); }
        @Override public void onAnimationEnd(Animator a) {
            mView.setHasTransientState(false);
            if (mListener != null) mListener.onAnimationEnd(a);
            Runnable r = mOnEnd.remove(a);
            if (r != null) r.run();
            mAnimators.remove(a);
        }
        @Override public void onAnimationUpdate(ValueAnimator a) {
            Props b = mAnimators.get(a);
            if (b == null) return;
            float f = a.getAnimatedFraction();
            /* (by index: a view's setter that asks for a property of this animator takes it out of the list) */
            for (int i = 0; i < b.values.size(); i++) { float[] p = b.values.get(i); set((int) p[0], p[1] + p[2] * f); }
            if (mUpdateListener != null) mUpdateListener.onAnimationUpdate(a);
        }
    }
}
