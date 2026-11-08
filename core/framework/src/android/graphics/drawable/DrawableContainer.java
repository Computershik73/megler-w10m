package android.graphics.drawable;

import android.graphics.*;

public class DrawableContainer extends Drawable implements Drawable.Callback {
    final java.util.ArrayList<Drawable> mChildren = new java.util.ArrayList<>();
    int mCurIndex = -1;
    private int mAlpha = 255;
    private ColorFilter mFilter;
    public int addChild(Drawable d) { mChildren.add(d); if (d != null) { d.setCallback(this); d.setBounds(getBounds()); } return mChildren.size() - 1; }
    private int mEnterFade, mExitFade;
    /** Cross-fades between states: changes here are immediate, the durations kept for whoever asks. */
    public void setEnterFadeDuration(int ms) { mEnterFade = ms; }
    public void setExitFadeDuration(int ms) { mExitFade = ms; }
    public boolean selectDrawable(int i) {
        if (i == mCurIndex) return false;
        mCurIndex = i < mChildren.size() ? i : -1;
        Drawable d = getCurrent();
        if (d != null) { d.setBounds(getBounds()); d.setState(getState()); d.setLevel(getLevel()); d.setAlpha(mAlpha); if (mFilter != null) d.setColorFilter(mFilter); }
        invalidateSelf();
        return true;
    }
    @Override public Drawable getCurrent() { return mCurIndex >= 0 && mCurIndex < mChildren.size() ? mChildren.get(mCurIndex) : null; }
    @Override public void draw(Canvas c) { Drawable d = getCurrent(); if (d != null) d.draw(c); }
    @Override protected void onBoundsChange(Rect b) { Drawable d = getCurrent(); if (d != null) d.setBounds(b); }
    @Override public int getIntrinsicWidth() { Drawable d = getCurrent(); return d == null ? -1 : d.getIntrinsicWidth(); }
    @Override public int getIntrinsicHeight() { Drawable d = getCurrent(); return d == null ? -1 : d.getIntrinsicHeight(); }
    @Override public boolean getPadding(Rect p) { Drawable d = getCurrent(); return d != null ? d.getPadding(p) : super.getPadding(p); }
    @Override public void setAlpha(int a) { mAlpha = a; Drawable d = getCurrent(); if (d != null) d.setAlpha(a); }
    @Override public int getAlpha() { return mAlpha; }
    @Override public void setColorFilter(ColorFilter f) { mFilter = f; for (Drawable d : mChildren) if (d != null) d.setColorFilter(f); }
    @Override public void setTintList(android.content.res.ColorStateList t) { for (Drawable d : mChildren) if (d != null) d.setTintList(t); }
    @Override protected boolean onLevelChange(int l) { Drawable d = getCurrent(); return d != null && d.setLevel(l); }
    @Override protected boolean onStateChange(int[] s) { Drawable d = getCurrent(); return d != null && d.setState(s); }
    @Override public boolean isStateful() { return true; }
    public void invalidateDrawable(Drawable who) { if (who == getCurrent()) invalidateSelf(); }
    public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
    public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }

    /*
     * The container's constant state: Android's DrawableContainerState over its children as they are, for an app that reads
     * them through it -- Spotify's SecondaryButtonView takes its selector's children so (getChildren), and ended on the null
     * the base Drawable's getConstantState() gave -- and for a copy (newDrawable): a container of the same kind with each
     * child's own copy, or the child itself where it has no constant state, as Android's copy shares such a child. Android's
     * container has none where a child has none (canConstantState); this one has one whatever its children are, since
     * several of Megler's drawables have no constant state where Android's have.
     */
    private ContainerState mMeglerState;
    @Override public ConstantState getConstantState() {
        if (mMeglerState == null) mMeglerState = new ContainerState(this);
        return mMeglerState;
    }
    /** A new empty container of this one's kind, for a copy: its children are added by meglerAddCopy. */
    DrawableContainer meglerNewEmpty() { return new DrawableContainer(); }
    /** The copy of child `i` added to the copy `to`, as this kind of container adds one (with its state set, its levels, its duration). */
    void meglerAddCopy(DrawableContainer to, int i, Drawable child) { to.addChild(child); }
    final DrawableContainer meglerCopy(android.content.res.Resources res) {
        DrawableContainer c = meglerNewEmpty();
        for (int i = 0; i < mChildren.size(); i++) {
            Drawable d = mChildren.get(i);
            ConstantState s = d != null ? d.getConstantState() : null;
            meglerAddCopy(c, i, s != null ? s.newDrawable(res) : d);
        }
        c.mEnterFade = mEnterFade;
        c.mExitFade = mExitFade;
        if (mAlpha != 255) c.setAlpha(mAlpha);
        if (mFilter != null) c.setColorFilter(mFilter);
        return c;
    }
    static final class ContainerState extends DrawableContainerState {
        private final DrawableContainer mOwner;
        ContainerState(DrawableContainer owner) { mOwner = owner; }
        @Override public Drawable newDrawable() { return mOwner.meglerCopy(null); }
        @Override public Drawable newDrawable(android.content.res.Resources res) { return mOwner.meglerCopy(res); }
        @Override public int addChild(Drawable d) { return mOwner.addChild(d); }
        @Override public Drawable getChild(int i) { return i >= 0 && i < mOwner.mChildren.size() ? mOwner.mChildren.get(i) : null; }
        @Override public int getChildCount() { return mOwner.mChildren.size(); }
        @Override public Drawable[] getChildren() { return mOwner.mChildren.toArray(new Drawable[0]); }
        @Override public boolean canConstantState() {
            for (Drawable d : mOwner.mChildren) if (d == null || d.getConstantState() == null) return false;
            return true;
        }
        @Override public boolean isStateful() { return mOwner.isStateful(); }
        @Override public int getOpacity() { return mOwner.getOpacity(); }
        @Override public int getEnterFadeDuration() { return mOwner.mEnterFade; }
        @Override public int getExitFadeDuration() { return mOwner.mExitFade; }
        @Override public void setEnterFadeDuration(int ms) { mOwner.setEnterFadeDuration(ms); }
        @Override public void setExitFadeDuration(int ms) { mOwner.setExitFadeDuration(ms); }
        @Override public int getConstantWidth() { int w = -1; for (Drawable d : mOwner.mChildren) if (d != null) w = Math.max(w, d.getIntrinsicWidth()); return w; }
        @Override public int getConstantHeight() { int h = -1; for (Drawable d : mOwner.mChildren) if (d != null) h = Math.max(h, d.getIntrinsicHeight()); return h; }
        @Override public int getConstantMinimumWidth() { int w = 0; for (Drawable d : mOwner.mChildren) if (d != null) w = Math.max(w, d.getMinimumWidth()); return w; }
        @Override public int getConstantMinimumHeight() { int h = 0; for (Drawable d : mOwner.mChildren) if (d != null) h = Math.max(h, d.getMinimumHeight()); return h; }
    }
    // ---- generated by tools/compat/genstubs.py: signatures only
    public static abstract class DrawableContainerState extends android.graphics.drawable.Drawable.ConstantState {
        protected DrawableContainerState() { super(); }
        // ---- generated by tools/compat/fillmembers.py (DrawableContainerState): the platform's members this class does not write (signatures only)
        private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
        protected DrawableContainerState(android.graphics.drawable.DrawableContainer.DrawableContainerState p0, android.graphics.drawable.DrawableContainer p1, android.content.res.Resources p2) { this(); }
        public int addChild(android.graphics.drawable.Drawable p0) { return 0; }
        public boolean canConstantState() { return false; }
        protected void computeConstantSize() {}
        public android.graphics.drawable.Drawable getChild(int p0) { return null; }
        public int getChildCount() { return 0; }
        public android.graphics.drawable.Drawable[] getChildren() { return null; }
        public int getConstantHeight() { return 0; }
        public int getConstantMinimumHeight() { return 0; }
        public int getConstantMinimumWidth() { return 0; }
        public android.graphics.Rect getConstantPadding() { return null; }
        public int getConstantWidth() { return 0; }
        public int getEnterFadeDuration() { return (megler.Fill.get(meglerFill, "EnterFadeDuration") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "EnterFadeDuration") : 0); }
        public int getExitFadeDuration() { return (megler.Fill.get(meglerFill, "ExitFadeDuration") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "ExitFadeDuration") : 0); }
        public int getOpacity() { return 0; }
        public void growArray(int p0, int p1) {}
        public boolean isConstantSize() { return (megler.Fill.get(meglerFill, "ConstantSize") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "ConstantSize") : false); }
        public boolean isStateful() { return false; }
        public void setConstantSize(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "ConstantSize", Boolean.valueOf(p0)); }
        public void setEnterFadeDuration(int p0) { meglerFill = megler.Fill.put(meglerFill, "EnterFadeDuration", Integer.valueOf(p0)); }
        public void setExitFadeDuration(int p0) { meglerFill = megler.Fill.put(meglerFill, "ExitFadeDuration", Integer.valueOf(p0)); }
        public void setVariablePadding(boolean p0) {}
        // ---- end of generated members (DrawableContainerState)
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    public int getCurrentIndex() { return (megler.Fill.get(meglerFill, "CurrentIndex") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "CurrentIndex") : 0); }
    public void setCurrentIndex(int p0) { meglerFill = megler.Fill.put(meglerFill, "CurrentIndex", Integer.valueOf(p0)); }
    protected void updateDensity(android.content.res.Resources p0) {}
    // ---- end of generated members
}
