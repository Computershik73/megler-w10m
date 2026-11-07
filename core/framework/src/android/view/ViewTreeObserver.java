package android.view;

import java.util.concurrent.CopyOnWriteArrayList;

public final class ViewTreeObserver {
    public interface OnGlobalLayoutListener { void onGlobalLayout(); }
    public interface OnPreDrawListener { boolean onPreDraw(); }
    public interface OnDrawListener { void onDraw(); }
    public interface OnWindowFocusChangeListener { void onWindowFocusChanged(boolean f); }
    public interface OnWindowAttachListener { void onWindowAttached(); void onWindowDetached(); }
    public interface OnGlobalFocusChangeListener { void onGlobalFocusChanged(View oldFocus, View newFocus); }
    public interface OnScrollChangedListener { void onScrollChanged(); }
    public interface OnTouchModeChangeListener { void onTouchModeChanged(boolean inTouchMode); }
    public interface OnWindowVisibilityChangeListener { void onWindowVisibilityChanged(int v); }
    private final CopyOnWriteArrayList<OnGlobalLayoutListener> mLayout = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnPreDrawListener> mPreDraw = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnDrawListener> mDraw = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowFocusChangeListener> mFocus = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowAttachListener> mAttach = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnGlobalFocusChangeListener> mGlobalFocus = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnScrollChangedListener> mScroll = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnTouchModeChangeListener> mTouchMode = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<Runnable> mFrameCommit = new CopyOnWriteArrayList<>();
    /*
     * A view's own observer, made while it was in no window, is merged into its window's when it is attached, and is dead
     * from then on (isAlive false), as on Android, where AndroidX's OneShotPreDrawListener and its kind then take the view's
     * observer anew to remove themselves. isAlive was always true, so such listeners removed themselves from the dead one
     * and stayed in the window's, run at every layout or frame (likely what YouTube's warnings "TraceableOnGlobalLayoutListener
     * .onGlobalLayout[ogn] is called 2" to "274" while an ad played say, package 606). What is still added to or removed from
     * a dead one goes to the one it was merged into, as Android's would only throw IllegalStateException.
     */
    private volatile boolean mAlive = true;
    private ViewTreeObserver mMergedInto;
    public ViewTreeObserver() {}
    void merge(ViewTreeObserver o) {
        mLayout.addAll(o.mLayout); mPreDraw.addAll(o.mPreDraw); mDraw.addAll(o.mDraw); mFocus.addAll(o.mFocus); mAttach.addAll(o.mAttach);
        mGlobalFocus.addAll(o.mGlobalFocus); mScroll.addAll(o.mScroll); mTouchMode.addAll(o.mTouchMode); mFrameCommit.addAll(o.mFrameCommit);
        o.mMergedInto = this;
        o.mAlive = false;
    }
    public boolean isAlive() { return mAlive; }
    /** The observer listeners are added to and removed from: this one, or the one this dead one was merged into. */
    private ViewTreeObserver live() {
        ViewTreeObserver o = this;
        while (!o.mAlive && o.mMergedInto != null) o = o.mMergedInto;
        return o;
    }
    public void addOnGlobalLayoutListener(OnGlobalLayoutListener l) { live().mLayout.add(l); }
    public void removeOnGlobalLayoutListener(OnGlobalLayoutListener l) { live().mLayout.remove(l); }
    @Deprecated public void removeGlobalOnLayoutListener(OnGlobalLayoutListener l) { live().mLayout.remove(l); }
    public void addOnPreDrawListener(OnPreDrawListener l) { live().mPreDraw.add(l); }
    public void removeOnPreDrawListener(OnPreDrawListener l) { live().mPreDraw.remove(l); }
    public void addOnDrawListener(OnDrawListener l) { live().mDraw.add(l); }
    public void removeOnDrawListener(OnDrawListener l) { live().mDraw.remove(l); }
    public void addOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { live().mFocus.add(l); }
    public void removeOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { live().mFocus.remove(l); }
    public void addOnWindowAttachListener(OnWindowAttachListener l) { live().mAttach.add(l); }
    public void removeOnWindowAttachListener(OnWindowAttachListener l) { live().mAttach.remove(l); }
    public void addOnGlobalFocusChangeListener(OnGlobalFocusChangeListener l) { live().mGlobalFocus.add(l); }
    public void removeOnGlobalFocusChangeListener(OnGlobalFocusChangeListener l) { live().mGlobalFocus.remove(l); }
    public void addOnScrollChangedListener(OnScrollChangedListener l) { live().mScroll.add(l); }
    public void removeOnScrollChangedListener(OnScrollChangedListener l) { live().mScroll.remove(l); }
    public void addOnTouchModeChangeListener(OnTouchModeChangeListener l) { live().mTouchMode.add(l); }
    public void removeOnTouchModeChangeListener(OnTouchModeChangeListener l) { live().mTouchMode.remove(l); }
    public void addOnWindowVisibilityChangeListener(OnWindowVisibilityChangeListener l) {}
    public void removeOnWindowVisibilityChangeListener(OnWindowVisibilityChangeListener l) {}
    public void registerFrameCommitCallback(Runnable r) { live().mFrameCommit.add(r); }
    public boolean unregisterFrameCommitCallback(Runnable r) { return live().mFrameCommit.remove(r); }
    public final void dispatchOnGlobalLayout() { for (OnGlobalLayoutListener l : mLayout) l.onGlobalLayout(); }
    void dispatchGlobalLayout() { dispatchOnGlobalLayout(); }
    public final boolean dispatchOnPreDraw() { boolean cancel = false; for (OnPreDrawListener l : mPreDraw) cancel |= !l.onPreDraw(); return cancel; }
    public final void dispatchOnDraw() { for (OnDrawListener l : mDraw) l.onDraw(); for (Runnable r : mFrameCommit) r.run(); mFrameCommit.clear(); }
    final void dispatchOnWindowFocusChange(boolean f) { for (OnWindowFocusChangeListener l : mFocus) l.onWindowFocusChanged(f); }
    final void dispatchOnWindowAttachedChange(boolean a) { for (OnWindowAttachListener l : mAttach) { if (a) l.onWindowAttached(); else l.onWindowDetached(); } }
    final void dispatchOnGlobalFocusChange(View o, View n) { for (OnGlobalFocusChangeListener l : mGlobalFocus) l.onGlobalFocusChanged(o, n); }
    final void dispatchOnScrollChanged() { for (OnScrollChangedListener l : mScroll) l.onScrollChanged(); }
    /** For megler.ViewRoot. */
    public void meglerWindowFocus(boolean f) { dispatchOnWindowFocusChange(f); }
    public void meglerAttached(boolean a) { dispatchOnWindowAttachedChange(a); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public void addOnComputeInternalInsetsListener(android.view.ViewTreeObserver.OnComputeInternalInsetsListener p0) {}
    public void addOnEnterAnimationCompleteListener(android.view.ViewTreeObserver.OnEnterAnimationCompleteListener p0) {}
    public void addOnSystemGestureExclusionRectsChangedListener(java.util.function.Consumer p0) {}
    public void addOnWindowShownListener(android.view.ViewTreeObserver.OnWindowShownListener p0) {}
    public void dispatchOnEnterAnimationComplete() {}
    public void dispatchOnWindowShown() {}
    public void removeOnComputeInternalInsetsListener(android.view.ViewTreeObserver.OnComputeInternalInsetsListener p0) {}
    public void removeOnEnterAnimationCompleteListener(android.view.ViewTreeObserver.OnEnterAnimationCompleteListener p0) {}
    public void removeOnSystemGestureExclusionRectsChangedListener(java.util.function.Consumer p0) {}
    public void removeOnWindowShownListener(android.view.ViewTreeObserver.OnWindowShownListener p0) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static final class InternalInsetsInfo {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public static final int TOUCHABLE_INSETS_CONTENT = 1;
        public static final int TOUCHABLE_INSETS_FRAME = 0;
        public static final int TOUCHABLE_INSETS_REGION = 3;
        public static final int TOUCHABLE_INSETS_VISIBLE = 2;
        public android.graphics.Rect contentInsets;
        public android.graphics.Region touchableRegion;
        public android.graphics.Rect visibleInsets;
        public InternalInsetsInfo() {}
        public void setTouchableInsets(int p0) { meglerProps.put("TouchableInsets", Integer.valueOf(p0)); }
    }
    public interface OnComputeInternalInsetsListener {
        void onComputeInternalInsets(android.view.ViewTreeObserver.InternalInsetsInfo p0);
    }
    public interface OnEnterAnimationCompleteListener {
        void onEnterAnimationComplete();
    }
    public interface OnWindowShownListener {
        void onWindowShown();
    }
    // ---- end of generated nested classes
}
