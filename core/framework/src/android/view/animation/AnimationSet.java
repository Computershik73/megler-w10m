package android.view.animation;
import java.util.ArrayList;
import java.util.List;
/*
 * Animations run together on one view, as Android's AnimationSet runs them:
 *  - Its listener hears the start as the first of its animations starts and the end as the last of them has ended, told
 *    inside getTransformation -- inside the drawing of its view, where Android's set calls dispatchAnimationStart and
 *    dispatchAnimationEnd itself, whatever handler the view gave it (a single animation's goes through that handler:
 *    Animation.fireStart). A set here told its listener nothing, ever. androidx's fragments finish taking a fragment's view
 *    away from that call (DefaultSpecialEffectsController: the view leaves with an EndViewTransitionAnimation, a set, whose
 *    listener's onAnimationEnd ends the view's transition and completes the operation), and react-native-screens wraps
 *    every screen's animation in a set of its own (ScreensCoordinatorLayout.startAnimation) whose listener lets the
 *    stack end the leaving screen's transition, which it refuses until then (ScreenStack.endViewTransition), and tells the
 *    JavaScript side that the new screen has appeared. With no such call the leaving screen stays the stack's disappearing
 *    child, drawn over the new one, and the new screen's appearing is never told: Discord showed its background picture
 *    and the keyboard of a field and nothing else (0.1.0.690, a user's phone).
 *  - What the set itself was given -- a duration, a start offset, fillBefore, fillAfter, a repeat mode: by its setters,
 *    which its XML's attributes call when they are there (megler.ResInflate) -- goes to each of its animations as the set is
 *    initialized, and nothing else does (mFlags): a set given no duration leaves each animation its own. Here setDuration
 *    changed the animations at once, and a set from XML gave all of them the longest's duration with its start offset in it.
 *    A start offset is added to each animation's own, which reset() puts back.
 *  - getDuration is the set's own if it was given one and the longest of its animations' otherwise; computeDurationHint the
 *    longest of theirs; getStartTime the earliest of theirs.
 */
public class AnimationSet extends Animation {
    private static final int FILL_AFTER = 1, FILL_BEFORE = 2, REPEAT_MODE = 4, START_OFFSET = 8, SHARE_INTERPOLATOR = 0x10, DURATION = 0x20, MATRIX = 0x40, BOUNDS = 0x80;
    private int mFlags;
    private ArrayList<Animation> mAnims = new ArrayList<>();
    private Transformation mTmp = new Transformation();
    private long mLastEnd;
    private long[] mStoredOffsets;
    public AnimationSet(boolean shareInterpolator) { if (shareInterpolator) mFlags |= SHARE_INTERPOLATOR; }
    public AnimationSet(android.content.Context c, android.util.AttributeSet a) { this(true); }
    @Override public void setFillAfter(boolean f) { mFlags |= FILL_AFTER; super.setFillAfter(f); }
    @Override public void setFillBefore(boolean f) { mFlags |= FILL_BEFORE; super.setFillBefore(f); }
    @Override public void setRepeatMode(int m) { mFlags |= REPEAT_MODE; super.setRepeatMode(m); }
    @Override public void setStartOffset(long o) { mFlags |= START_OFFSET; super.setStartOffset(o); }
    @Override public void setDuration(long d) { mFlags |= DURATION; super.setDuration(d); mLastEnd = mStartOffset + mDuration; }
    public void addAnimation(Animation a) {
        mAnims.add(a);
        if (a.willChangeTransformationMatrix()) mFlags |= MATRIX;
        if (a.willChangeBounds()) mFlags |= BOUNDS;
        if ((mFlags & DURATION) != 0) mLastEnd = mStartOffset + mDuration;
        else if (mAnims.size() == 1) { mDuration = a.getStartOffset() + a.getDuration(); mLastEnd = mStartOffset + mDuration; }
        else { mLastEnd = Math.max(mLastEnd, mStartOffset + a.getStartOffset() + a.getDuration()); mDuration = mLastEnd - mStartOffset; }
    }
    public List<Animation> getAnimations() { return mAnims; }
    @Override public long getDuration() {
        if ((mFlags & DURATION) != 0) return mDuration;
        long d = 0;
        for (Animation a : mAnims) d = Math.max(d, a.getDuration());
        return d;
    }
    @Override public long computeDurationHint() { long d = 0; for (Animation a : mAnims) d = Math.max(d, a.computeDurationHint()); return d; }
    @Override public long getStartTime() { long t = Long.MAX_VALUE; for (Animation a : mAnims) t = Math.min(t, a.getStartTime()); return t; }
    @Override public void setStartTime(long t) { super.setStartTime(t); for (Animation a : mAnims) a.setStartTime(t); }
    @Override public void restrictDuration(long d) { super.restrictDuration(d); for (Animation a : mAnims) a.restrictDuration(d); }
    @Override public void scaleCurrentDuration(float s) { for (Animation a : mAnims) a.scaleCurrentDuration(s); }
    @Override public boolean willChangeTransformationMatrix() { return (mFlags & MATRIX) != 0; }
    @Override public boolean willChangeBounds() { return (mFlags & BOUNDS) != 0; }
    @Override public void initialize(int w, int h, int pw, int ph) {
        super.initialize(w, h, pw, ph);
        boolean share = (mFlags & SHARE_INTERPOLATOR) != 0, offset = (mFlags & START_OFFSET) != 0;
        if (share) ensureInterpolator();
        int n = mAnims.size();
        if (!offset) mStoredOffsets = null;
        else if (mStoredOffsets == null || mStoredOffsets.length != n) mStoredOffsets = new long[n];
        for (int i = 0; i < n; i++) {
            Animation a = mAnims.get(i);
            if ((mFlags & DURATION) != 0) a.setDuration(mDuration);
            if ((mFlags & FILL_AFTER) != 0) a.setFillAfter(mFillAfter);
            if ((mFlags & FILL_BEFORE) != 0) a.setFillBefore(mFillBefore);
            if ((mFlags & REPEAT_MODE) != 0) a.setRepeatMode(mRepeatMode);
            if (share) a.setInterpolator(mInterpolator);
            if (offset) { long own = a.getStartOffset(); a.setStartOffset(own + mStartOffset); mStoredOffsets[i] = own; }
            a.initialize(w, h, pw, ph);
        }
    }
    /* (The animations' own start offsets back, as before the set added its own; and each animation reset with it, as here a reset is a new start.) */
    @Override public void reset() {
        super.reset();
        long[] own = mStoredOffsets;
        if (own != null) for (int i = 0; i < own.length && i < mAnims.size(); i++) mAnims.get(i).setStartOffset(own[i]);
        for (Animation a : mAnims) a.reset();
    }
    @Override public boolean getTransformation(long now, Transformation out) {
        boolean more = false, started = false, ended = true;
        out.clear();
        for (int i = mAnims.size() - 1; i >= 0; i--) {
            Animation a = mAnims.get(i);
            mTmp.clear();
            more = a.getTransformation(now, mTmp, getScaleFactor()) || more;
            out.compose(mTmp);
            started = started || a.hasStarted();
            ended = a.hasEnded() && ended;
        }
        // (Each flag set before its call: a listener that clears the view's animation from it is not told the end twice, Animation.cancel.)
        if (started && !mStarted) { mStarted = true; tellStart(); }
        if (ended && !mEnded) { mEnded = true; tellEnd(); }
        else if (!ended) mEnded = false;
        return more;
    }
    @Override protected AnimationSet clone() throws CloneNotSupportedException {
        AnimationSet s = (AnimationSet) super.clone();
        s.mTmp = new Transformation();
        s.mAnims = new ArrayList<>();
        for (Animation a : mAnims) s.mAnims.add(a.clone());
        if (mStoredOffsets != null) s.mStoredOffsets = mStoredOffsets.clone();
        return s;
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public int getExtensionEdges() { return 0; }
    public void getTransformationAt(float p0, android.view.animation.Transformation p1) {}
    public boolean hasAlpha() { return false; }
    public void initializeInvalidateRegion(int p0, int p1, int p2, int p3) {}
    // ---- end of generated members
}
