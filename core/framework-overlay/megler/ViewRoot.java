// megler-w10m overlay of Megler/src/java-framework/src/megler/ViewRoot.java (tools/build-framework.sh).
// It differs in three calls of megler.Native.frameMark in doFrame, which tell the Java driver where a frame's measure and
// layout begin, where its draw traversal begins, and where it ends with how much of the screen it drew (sRedrawn, a field
// drawAll sets).
//
// And in what a frame draws. Each window keeps the rectangle its views asked to have drawn again (mDirty: View.meglerDirty,
// in the overlay's View, passes each invalidation up as a rectangle, clipped as it goes), and drawAll draws only the union
// of those: the screen's canvas clipped to it and cleared there, every window drawn through that clip (View.drawFromParent
// skips children wholly outside it), and the rectangle told to the shell with the picture (Native.presentRect). A frame in
// which nothing visible changed draws nothing and presents nothing. The whole screen is drawn as before when a window was
// added, removed, moved or resized, when a window asked without a rectangle (ViewRoot.invalidate, invalidateFromAnyThread),
// and at the first frame. The original drew the whole screen in every frame: Android's software rendering draws the dirty
// region only (ViewRootImpl.mDirty), and so a blinking cursor cost a whole screen here. For the tests (Native.checkPartial),
// checkPart draws each partial frame again whole into a second bitmap and says what differs outside the part.
//
// And in keyFocus(), which the original does not have: the focused view of the window keys go to, which the overlay's
// megler.InputMethods names in the log when typed text arrives.
//
// And in what the keyboard does to a window, which is now Android's: the window's softInputMode decides (adjustMode).
//  - adjustResize: an activity's window gets the keyboard in its insets, so the decor keeps the content above it, as before;
//    a dialog is measured to fit the screen above the keyboard and placed in that by its gravity (centred, usually), and
//    its views get no keyboard inset, since none of it is under the keyboard.
//  - adjustPan: the window keeps its size and place and is moved up (mPan) only as far as brings the focused view's bottom
//    edge above the keyboard, and no further than its top edge to the top of the screen.
//  - adjustUnspecified: resize when the window has a scroll container (ScrollView, a list, a view set as one), pan when not.
//  - adjustNothing: nothing.
// The original laid every dialog out against the whole screen and placed it by gravity in the part above the keyboard, so
// a dialog taller than that part went off the top: on the phone Apktool M's search dialog had its field above the screen's
// top edge while the user typed into it. And it gave a dialog the part of it the keyboard covered as an inset, which a view
// that fits system windows turned into padding, making the dialog taller still. An activity was always resized.
//
// Why. Where a frame's time goes -- in Java and in which part of the frame, in the shell's drawing, in handing the picture
// over -- is measured from the core's side (core/src/drivers/hk_java.c, "the frame meter", a line in the log every five
// seconds while the app draws), and only the frame itself knows where its phases begin. Nothing else changes: the calls
// are native and take no time to speak of. Everything else is the original's, unchanged.
package megler;

import android.graphics.*;
import android.os.Handler;
import android.os.Looper;
import android.view.*;
import java.util.ArrayList;

/**
 * A window's root, as ViewRootImpl: it lays out its view tree to the window's size, and with the other windows draws it into the
 * screen's one bitmap, which the render thread puts over the app's GL (megler.Gfx.present). Touches go to the topmost window that takes
 * them, keys to the topmost focusable one. Everything here runs on the main thread.
 */
public final class ViewRoot implements ViewParent {
    private static final ArrayList<ViewRoot> sRoots = new ArrayList<>();
    private static Bitmap sScreen;
    private static Canvas sCanvas;
    private static boolean sScheduled, sPresentedAny;
    private static Handler sMain;
    private static int sNextViewId = 0x00100000;
    private static int[] sInsets = { 0, 0, 0, 0 };     /* left, top, right, bottom: the notch / status bar and home indicator areas */
    private static int sImeBottom;
    private static ViewRoot sTouchTarget;

    final View mView;
    WindowManager.LayoutParams mLp;
    private int mX, mY, mW, mH;
    /* Where the layout put the window, and how far the keyboard pans it up from there (adjustPan): mY is the one less the other. */
    private int mLaidY, mPan;
    /* What the keyboard does to the window (adjustMode), as found when its insets were last worked out. */
    private int mAdjust = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
    private boolean mDumpTimer;
    private boolean mLayoutRequested = true, mFirst = true, mRemoved, mHasFocus = true, mInLayout, mInsetsDirty = true;
    /** What of this window is to be drawn again, in its own coordinates; or all of it. */
    private final Rect mDirty = new Rect();
    private boolean mDirtyAll = true;
    /** The whole screen is to be drawn at the next frame: a window came, went, moved or changed its size. */
    private static boolean sFullRedraw = true, sDrawingPart;
    private static final Rect sPart = new Rect(), sScratch = new Rect();
    private View mFocused;
    private final ViewTreeObserver mObserver = new ViewTreeObserver();
    private final android.os.IBinder mToken = new android.os.Binder();
    private WindowInsets mWindowInsets;
    private int mSystemUi;
    private final KeyEvent.DispatcherState mKeyState = new KeyEvent.DispatcherState();
    private WindowInsetsController mInsetsController;

    private ViewRoot(View v, WindowManager.LayoutParams lp) { mView = v; mLp = new WindowManager.LayoutParams(); mLp.copyFrom(lp); }

    public static Handler mainHandler() { if (sMain == null) sMain = new Handler(Looper.getMainLooper()); return sMain; }
    public static synchronized int nextViewId() { return sNextViewId++; }
    public static ViewRoot of(View v) { for (ViewRoot r : sRoots) if (r.mView == v) return r; return null; }
    public static ArrayList<ViewRoot> roots() { return sRoots; }
    public View getView() { return mView; }

    /** A new window on top of the others. */
    public static ViewRoot add(View v, WindowManager.LayoutParams lp) {
        ViewRoot r = new ViewRoot(v, lp);
        // An activity's own window goes under the windows it already opened (a dialog shown from onCreate is added before the
        // activity's window, which comes after onResume): on Android a dialog stays above the activity it belongs to.
        int at = sRoots.size();
        android.app.Activity owner = lp != null && lp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION ? activityOf(v.getContext()) : null;
        if (owner != null) for (int i = 0; i < sRoots.size(); i++) if (activityOf(sRoots.get(i).mView.getContext()) == owner) { at = i; break; }
        boolean under = at < sRoots.size();
        sRoots.add(at, r);
        if (under) for (int i = at + 1; i < sRoots.size(); i++) if (sRoots.get(i).focusable()) { r.mHasFocus = false; break; }
        if (v.getLayoutParams() == null) v.setLayoutParams(r.mLp);
        sFullRedraw = true;
        r.attach();
        android.webkit.WebView.meglerWindowsChanged();
        if (r.mHasFocus) for (ViewRoot o : sRoots) if (o != r && o.mHasFocus && r.focusable()) { o.mHasFocus = false; o.mView.dispatchWindowFocusChanged(false); o.mObserver.meglerWindowFocus(false); }
        scheduleFrame();
        return r;
    }
    private WindowInsets computeInsets() {
        boolean full = mLp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION || mLp.type == WindowManager.LayoutParams.TYPE_APPLICATION;
        mAdjust = adjustMode();
        boolean resize = mAdjust == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
        /* A dialog lies above the keyboard when it is resized and is moved, not shrunk, when it is panned: no part of it is an inset. */
        if (floatingAppWindow()) return WindowInsets.meglerOf(0, 0, 0, 0, 0, null);
        if (full) return WindowInsets.meglerOf(sInsets[0], sInsets[1], sInsets[2], sInsets[3], resize ? sImeBottom : 0, cutout());
        return WindowInsets.meglerOf(0, 0, 0, 0, Math.max(0, sImeBottom - (screenH() - mY - mH)), null);
    }

    /** An application's window that does not fill the screen: a dialog (Android lays these out in the space the keyboard leaves). */
    private boolean floatingAppWindow() {
        return mLp.type > WindowManager.LayoutParams.TYPE_BASE_APPLICATION && mLp.type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW
               && (mLp.width != ViewGroup.LayoutParams.MATCH_PARENT || mLp.height != ViewGroup.LayoutParams.MATCH_PARENT);
    }

    /**
     * What the keyboard does to this window, as Android's window manager decides it: the adjustment its softInputMode names,
     * and for one that names none, resize when a view of the window is a scroll container and pan when none is.
     */
    private int adjustMode() {
        int m = mLp.softInputMode & WindowManager.LayoutParams.SOFT_INPUT_MASK_ADJUST;
        if (m == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED)
            m = hasScrollContainer(mView) ? WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE : WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN;
        return m;
    }

    /* On Android a view with vertical scrollbars (ScrollView's and the lists' styles have them) or set so is a scroll container. */
    private static boolean hasScrollContainer(View v) {
        if (v.isScrollContainer() || v instanceof android.widget.ScrollView || v instanceof android.widget.AbsListView) return true;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) if (hasScrollContainer(g.getChildAt(i))) return true;
        }
        return false;
    }

    /**
     * adjustPan: how far the window is moved up so that the focused view is above the keyboard -- as far as brings its bottom
     * edge to the keyboard's top, no further than brings its top edge to the screen's top -- or 0. Asked at every frame, since
     * the focus can move without a layout.
     */
    private void updatePan() {
        int pan = 0;
        if (sImeBottom > 0 && mAdjust == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN && mHasFocus && mFocused != null && mFocused.isShown()) {
            int[] at = new int[2];
            mFocused.getLocationInWindow(at);
            int top = mLaidY + at[1], bottom = top + mFocused.getHeight(), visible = screenH() - sImeBottom;
            if (bottom > visible) pan = Math.max(0, Math.min(bottom - visible, top));
        }
        if (pan != mPan) {
            mPan = pan;
            mY = mLaidY - mPan;
            sFullRedraw = true;
        }
    }
    private void attach() {
        // as ViewRootImpl: the window's insets are known by the time its views hear they are attached (getRootWindowInsets)
        if (mWindowInsets == null) mWindowInsets = computeInsets();
        mView.meglerAttach(this);
        mObserver.meglerAttached(true);
    }
    public void remove() {
        if (mRemoved) return;
        mRemoved = true;
        sFullRedraw = true;
        boolean hadFocus = mHasFocus;
        sRoots.remove(this);
        if (sTouchTarget == this) sTouchTarget = null;
        mView.meglerDetach();
        android.webkit.WebView.meglerWindowsChanged();
        mObserver.meglerAttached(false);
        if (hadFocus) {
            for (int i = sRoots.size() - 1; i >= 0; i--) {
                ViewRoot o = sRoots.get(i);
                if (o.focusable()) { if (!o.mHasFocus) { o.mHasFocus = true; o.mView.dispatchWindowFocusChanged(true); o.mObserver.meglerWindowFocus(true); } break; }
            }
        }
        scheduleFrame();
    }
    public void setLayoutParams(WindowManager.LayoutParams lp) { mLp.copyFrom(lp); mView.setLayoutParams(mLp); mLayoutRequested = true; sFullRedraw = true; scheduleFrame(); }
    public WindowManager.LayoutParams layoutParams() { return mLp; }
    private boolean focusable() { return (mLp.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0; }

    // ---- what views ask of their root
    public ViewTreeObserver observer() { return mObserver; }
    public Handler handler() { return mainHandler(); }
    public android.os.IBinder token() { return mToken; }
    public View focused() { return mFocused; }
    public void setFocused(View v) {
        View old = mFocused;
        mFocused = v;
        if (old != v) InputMethods.focusChanged(old, v);
    }
    public boolean hasWindowFocus() { return mHasFocus; }
    public boolean isInLayout() { return mInLayout; }
    public int width() { return mW; }
    public int height() { return mH; }
    public int windowX() { return mX; }
    public int windowY() { return mY; }
    public KeyEvent.DispatcherState keyDispatcherState() { return mKeyState; }
    public void keepScreenOn(boolean on) {}
    /** All of the window is to be drawn again. */
    public void invalidate() { mDirtyAll = true; scheduleFrame(); }
    public void invalidateFromAnyThread() { if (Looper.myLooper() == Looper.getMainLooper()) invalidate(); else mainHandler().post(this::invalidate); }
    /** A rectangle of the window, in its own coordinates (View.meglerDirty), is to be drawn again; nothing when none of it is in the window. */
    public void meglerDirty(RectF box) {
        if (!mDirtyAll) {
            int l = Math.max(0, (int) Math.floor(box.left)), t = Math.max(0, (int) Math.floor(box.top));
            int r = Math.min(mW, (int) Math.ceil(box.right)), b = Math.min(mH, (int) Math.ceil(box.bottom));
            if (l >= r || t >= b) return;
            mDirty.union(l, t, r, b);
        }
        scheduleFrame();
    }
    /** Whether the frame being drawn draws only part of the screen (View.drawFromParent then skips what lies outside it). */
    public static boolean meglerDrawingPart() { return sDrawingPart; }
    public void requestLayout() { mLayoutRequested = true; scheduleFrame(); }
    public void requestApplyInsets() { mInsetsDirty = true; mLayoutRequested = true; scheduleFrame(); }
    public WindowInsets insets() { return mWindowInsets; }
    public int systemUi() { return mSystemUi; }
    public void systemUiChanged(int v) { mSystemUi = v; requestApplyInsets(); }
    public void visibleDisplayFrame(Rect r) { r.set(sInsets[0], sInsets[1], screenW() - sInsets[2], screenH() - Math.max(sInsets[3], sImeBottom)); }
    public WindowInsetsController insetsController() {
        if (mInsetsController == null) mInsetsController = new WindowInsetsController() {
            int appearance, behavior;
            public void show(int types) { if ((types & WindowInsets.Type.ime()) != 0 && mFocused != null) InputMethods.show(mFocused); }
            public void hide(int types) { if ((types & WindowInsets.Type.ime()) != 0) InputMethods.hide(); }
            public void setSystemBarsAppearance(int a, int mask) { appearance = (appearance & ~mask) | (a & mask); }
            public int getSystemBarsAppearance() { return appearance; }
            public void setSystemBarsBehavior(int b) { behavior = b; }
            public int getSystemBarsBehavior() { return behavior; }
        };
        return mInsetsController;
    }

    // ---- insets from the host
    /** The screen's insets in pixels (Megler's host tells them), and the keyboard's height. */
    public static void setInsets(int l, int t, int r, int b) { sInsets = new int[] { l, t, r, b }; for (ViewRoot v : sRoots) v.requestApplyInsets(); }
    public static void setImeHeight(int h) { if (h != sImeBottom) { sImeBottom = h; for (ViewRoot v : sRoots) v.requestApplyInsets(); } }
    public static int imeHeight() { return sImeBottom; }
    public static int[] screenInsetsMegler() { return sInsets; }
    public static WindowInsets screenInsets() { return WindowInsets.meglerOf(sInsets[0], sInsets[1], sInsets[2], sInsets[3], sImeBottom, cutout()); }
    public static DisplayCutout cutout() { return sInsets[1] > 0 ? new DisplayCutout(new Rect(0, sInsets[1], 0, 0), null) : null; }
    static int screenW() { return Native.screenWidth(); }
    static int screenH() { return Native.screenHeight(); }

    // ---- the frame
    public static void scheduleFrame() {
        if (sScheduled) return;
        sScheduled = true;
        Choreographer.getMainThreadInstance().postCallback(Choreographer.CALLBACK_TRAVERSAL, ViewRoot::doFrame, null);
    }
    private static void doFrame() {
        Native.frameMark(1, 0);
        sScheduled = false;
        ArrayList<ViewRoot> roots = new ArrayList<>(sRoots);
        for (ViewRoot r : roots) if (!r.mRemoved) r.performLayout();
        for (ViewRoot r : roots) if (!r.mRemoved && r.mObserver.dispatchOnPreDraw()) { scheduleFrame(); }
        Native.frameMark(2, 0);
        drawAll(roots);
        for (ViewRoot r : roots) if (!r.mRemoved) r.mObserver.dispatchOnDraw();
        Native.frameMark(3, sRedrawn);
    }
    /** Pixels of the screen the last drawAll drew: its dirty rectangle, or all of them when it drew the whole screen. */
    private static int sRedrawn;
    private static Boolean sDump;
    private long mLastDump;
    /** TL_VIEW_DUMP: the window's views, with their frames, to the log (every 3 s while it lays out). */
    private static void dump(View v, int depth) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < depth; i++) b.append("  ");
        b.append(v.getClass().getName()).append(' ').append(v.getLeft()).append(',').append(v.getTop()).append(' ').append(v.getWidth()).append('x').append(v.getHeight());
        if (v.getVisibility() != View.VISIBLE) b.append(v.getVisibility() == View.GONE ? " GONE" : " INVISIBLE");
        if (v.getAlpha() != 1f) b.append(" a=").append(v.getAlpha());
        if (v.getLayerType() != View.LAYER_TYPE_NONE) b.append(" layer=").append(v.getLayerType());
        if (v.getId() != View.NO_ID) b.append(" #").append(Integer.toHexString(v.getId()));
        if (v instanceof android.widget.TextView) {
            CharSequence tx = ((android.widget.TextView) v).getText();
            StringBuilder e = new StringBuilder();
            for (int i = 0; tx != null && i < tx.length(); i++) { char ch = tx.charAt(i); if (ch < 32 || ch > 126) e.append(String.format("\\u%04x", (int) ch)); else e.append(ch); }
            b.append(" \"").append(e).append('"');
            if (tx instanceof android.text.Spanned) for (Object sp : ((android.text.Spanned) tx).getSpans(0, tx.length(), Object.class)) {
                b.append(" span=").append(sp.getClass().getName());
                if (sp instanceof android.text.style.DynamicDrawableSpan) { android.graphics.drawable.Drawable dd = ((android.text.style.DynamicDrawableSpan) sp).getDrawable(); b.append('(').append(dd == null ? "null" : dd.getClass().getName() + " " + dd.getBounds() + " cf=" + dd.getColorFilter()).append(')'); }
            }
            android.text.TextPaint tp = ((android.widget.TextView) v).getPaint();
            b.append(" color=").append(Integer.toHexString(tp.getColor())).append(tp.getShader() != null ? " shader=" + tp.getShader().getClass().getSimpleName() : "")
             .append(tp.getXfermode() != null ? " xfer" : "").append(tp.getColorFilter() != null ? " cf" : "").append(tp.getMaskFilter() != null ? " mask" : "");
        }
        android.graphics.drawable.Drawable bg = v.getBackground();
        if (bg instanceof android.graphics.drawable.InsetDrawable && ((android.graphics.drawable.InsetDrawable) bg).getDrawable() != null) {
            b.append(" bg=Inset(");
            bg = ((android.graphics.drawable.InsetDrawable) bg).getDrawable();
        }
        if (bg != null) {
            b.append(" bg=").append(bg.getClass().getSimpleName());
            if (bg instanceof android.graphics.drawable.ColorDrawable) b.append('#').append(Integer.toHexString(((android.graphics.drawable.ColorDrawable) bg).getColor()));
            if (bg instanceof android.graphics.drawable.GradientDrawable && ((android.graphics.drawable.GradientDrawable) bg).getColor() != null) b.append(((android.graphics.drawable.GradientDrawable) bg).getColor());
            b.append(" alpha=").append(bg.getAlpha());
            if (v.getBackgroundTintList() != null) b.append(" tint=").append(v.getBackgroundTintList());
        }
        android.view.ViewGroup.LayoutParams lp = v.getLayoutParams();
        if (lp != null) {
            b.append(" lp=").append(lp.getClass().getSimpleName()).append('(').append(lp.width).append(',').append(lp.height).append(')');
            for (String f : new String[] { "topToTop", "topToBottom", "bottomToBottom", "leftToLeft", "startToStart", "endToEnd", "rightToRight", "horizontalBias", "dimensionRatio" }) {
                try { java.lang.reflect.Field fl = lp.getClass().getField(f); Object o = fl.get(lp); if (o != null && !"-1".equals(String.valueOf(o)) && !"0.5".equals(String.valueOf(o))) b.append(' ').append(f).append('=').append(o); } catch (Exception e) {}
            }
        }
        android.util.Log.d("ViewDump", b.toString());
        if (v instanceof android.view.ViewGroup) { android.view.ViewGroup g = (android.view.ViewGroup) v; for (int i = 0; i < g.getChildCount(); i++) dump(g.getChildAt(i), depth + 1); }
    }
    private void performLayout() {
        int sw = screenW(), sh = screenH();
        if (mInsetsDirty || mFirst) {
            mInsetsDirty = false;
            mWindowInsets = computeInsets();
            mView.dispatchApplyWindowInsets(mWindowInsets);
            mLayoutRequested = true;
        }
        if (!mLayoutRequested && !mView.isLayoutRequested()) {
            updatePan();
            return;
        }
        mLayoutRequested = false;
        mInLayout = true;
        try {
            int ws, hs;
            int lw = mLp.width, lh = mLp.height;
            int maxW = sw, maxH = sh;
            boolean resize = sImeBottom > 0 && mAdjust == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
            /* A dialog resized for the keyboard is laid out in the space above it, as Android's window manager fits its frame there. */
            if (resize && floatingAppWindow()) maxH = Math.max(0, sh - sInsets[1] - Math.max(sInsets[3], sImeBottom));
            if (lw == ViewGroup.LayoutParams.MATCH_PARENT) ws = View.MeasureSpec.makeMeasureSpec(maxW, View.MeasureSpec.EXACTLY);
            else if (lw == ViewGroup.LayoutParams.WRAP_CONTENT) ws = View.MeasureSpec.makeMeasureSpec(maxW, View.MeasureSpec.AT_MOST);
            else ws = View.MeasureSpec.makeMeasureSpec(lw, View.MeasureSpec.EXACTLY);
            if (lh == ViewGroup.LayoutParams.MATCH_PARENT) hs = View.MeasureSpec.makeMeasureSpec(maxH, View.MeasureSpec.EXACTLY);
            else if (lh == ViewGroup.LayoutParams.WRAP_CONTENT) hs = View.MeasureSpec.makeMeasureSpec(maxH, View.MeasureSpec.AT_MOST);
            else hs = View.MeasureSpec.makeMeasureSpec(lh, View.MeasureSpec.EXACTLY);
            mView.measure(ws, hs);
            int w = mView.getMeasuredWidth(), h = mView.getMeasuredHeight();
            // where the window goes: gravity and offsets for windows that do not fill the screen (dialogs, popups, toasts)
            int g = mLp.gravity;
            Rect out = new Rect();
            if (g == 0 && (lw != ViewGroup.LayoutParams.MATCH_PARENT || lh != ViewGroup.LayoutParams.MATCH_PARENT)) g = Gravity.TOP | Gravity.LEFT;
            if (g == 0) out.set(0, 0, w, h);
            else {
                Rect area = new Rect(0, 0, sw, sh);
                /* (A dialog that is panned is placed as if there were no keyboard, and moved up afterwards: updatePan.) */
                int ime = floatingAppWindow() && !resize ? 0 : sImeBottom;
                if (mLp.type >= 1000 || (mLp.flags & WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS) == 0 && mLp.type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION)
                    area.set(sInsets[0], sInsets[1], sw - sInsets[2], sh - Math.max(sInsets[3], ime));
                Gravity.apply(g, w, h, area, mLp.x, mLp.y, out);
            }
            if (out.left != mX || out.top != mLaidY || w != mW || h != mH) sFullRedraw = true;
            mX = out.left; mLaidY = out.top; mY = mLaidY - mPan; mW = w; mH = h;
            mView.layout(0, 0, w, h);
        } finally { mInLayout = false; }
        mObserver.dispatchOnGlobalLayout();
        updatePan();
        if (sDump == null) sDump = System.getenv("TL_VIEW_DUMP") != null;
        if (sDump) {
            long now = android.os.SystemClock.uptimeMillis();
            if (now - mLastDump > 3000) { mLastDump = now; dump(mView, 0); }
            if (!mDumpTimer) {                  /* and again every 4 s, laid out again or not: a window that never relayouts shows why */
                mDumpTimer = true;
                final Runnable[] again = new Runnable[1];
                again[0] = () -> { if (mRemoved) return; android.util.Log.d("ViewDump", "---- " + mView.getClass().getName() + " (timer; layout requested " + mView.isLayoutRequested() + ")"); dump(mView, 0); mainHandler().postDelayed(again[0], 4000); };
                mainHandler().postDelayed(again[0], 4000);
            }
        }
        if (mFirst) {
            mFirst = false;
            if (mHasFocus) { mView.dispatchWindowFocusChanged(true); mObserver.meglerWindowFocus(true); }
            if (mView.findFocus() == null && mView instanceof ViewGroup) { /* the first focusable view takes focus when it asks; nothing forced */ }
        }
    }
    private static void drawAll(ArrayList<ViewRoot> roots) {
        int sw = screenW(), sh = screenH();
        boolean whole = sFullRedraw;
        if (sScreen == null || sScreen.getWidth() != sw || sScreen.getHeight() != sh) {
            if (sCanvas != null) sCanvas.meglerRelease();
            sScreen = Bitmap.createBitmap(sw, sh, Bitmap.Config.ARGB_8888);
            sCanvas = new Canvas(sScreen);
            whole = true;
        }
        // What the windows asked for, on the screen
        Rect part = sPart;
        part.setEmpty();
        for (ViewRoot r : roots) {
            if (r.mRemoved) continue;
            if (r.mDirtyAll) whole = true;
            else if (!r.mDirty.isEmpty()) { sScratch.set(r.mDirty); sScratch.offset(r.mX, r.mY); part.union(sScratch); }
        }
        for (ViewRoot r : roots) { r.mDirty.setEmpty(); r.mDirtyAll = false; }
        sFullRedraw = false;
        if (!whole) {
            sRedrawn = 0;
            if (!sPresentedAny || !part.intersect(0, 0, sw, sh)) return;      /* nothing visible changed: the screen is as it was */
            int save = sCanvas.save();
            sCanvas.clipRect(part);
            sCanvas.drawColor(0, PorterDuff.Mode.CLEAR);
            sDrawingPart = true;
            try { drawWindows(sCanvas, roots, sw, sh); } finally { sDrawingPart = false; sCanvas.restoreToCount(save); }
            sRedrawn = part.width() * part.height();
            if (sCheck == 0) sCheck = Native.checkPartial() ? 1 : -1;
            if (sCheck > 0) checkPart(roots, sw, sh, part);
            Native.presentRect(part.left, part.top, part.right, part.bottom);
            Gfx.present(sScreen.meglerNative());
            return;
        }
        sScreen.eraseColor(0);
        int gen = sScreen.getGenerationId();
        boolean drew = drawWindows(sCanvas, roots, sw, sh);
        // a window drawn counts even when its drawing left the generation alone (the screen must lose a closed dialog)
        boolean any = drew || sScreen.getGenerationId() != gen;
        sRedrawn = any ? sw * sh : 0;
        if (any || sPresentedAny) Gfx.present(any ? sScreen.meglerNative() : 0);
        sPresentedAny = any;
    }
    /** For the tests (Native.checkPartial): 1 a frame drawn in part is checked against the same frame drawn whole, -1 not, 0 not asked yet. */
    private static int sCheck;
    private static Bitmap sWhole;
    private static Canvas sWholeCanvas;
    private static final int[] sBox = new int[8];
    /**
     * The screen drawn whole into a bitmap of its own and compared, outside the part, with the screen drawn in part: a difference
     * there is what the dirty region missed. (Inside the part both are drawn, and differ only where drawing depends on the time;
     * so does a focused text field's cursor, whose field is left out too.)
     */
    private static void checkPart(ArrayList<ViewRoot> roots, int sw, int sh, Rect part) {
        if (sWhole == null || sWhole.getWidth() != sw || sWhole.getHeight() != sh) {
            if (sWholeCanvas != null) sWholeCanvas.meglerRelease();
            sWhole = Bitmap.createBitmap(sw, sh, Bitmap.Config.ARGB_8888);
            sWholeCanvas = new Canvas(sWhole);
        }
        sWhole.eraseColor(0);
        drawWindows(sWholeCanvas, roots, sw, sh);
        sBox[0] = part.left; sBox[1] = part.top; sBox[2] = part.right; sBox[3] = part.bottom;
        sBox[4] = sBox[5] = sBox[6] = sBox[7] = 0;
        // A focused text field blinks its cursor by the clock, on a timer of its own: drawn again now it can be in the other phase
        for (ViewRoot r : roots) {
            View f = r.mRemoved ? null : r.mFocused;
            if (!(f instanceof android.widget.TextView) || !f.isShown()) continue;
            int[] at = new int[2];
            f.getLocationOnScreen(at);
            sBox[4] = at[0]; sBox[5] = at[1]; sBox[6] = at[0] + f.getWidth(); sBox[7] = at[1] + f.getHeight();
        }
        int n = Gfx.bmDiffer(sScreen.meglerNative(), sWhole.meglerNative(), sBox);
        if (n != 0) android.util.Log.w("ViewRoot", "a frame drawn in part missed " + n + " pixels, in " + sBox[0] + "," + sBox[1] + "-" + sBox[2] + "," + sBox[3]
                + " (the part drawn was " + part.toShortString() + ")");
    }
    /** Every window, bottom first, into a canvas of the screen as it is clipped; whether any was drawn. */
    private static boolean drawWindows(Canvas c, ArrayList<ViewRoot> roots, int sw, int sh) {
        Paint dim = null;
        boolean drew = false;
        for (ViewRoot r : roots) {
            if (r.mRemoved || r.mView.getVisibility() != View.VISIBLE) continue;
            if ((r.mLp.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0 && r.mLp.dimAmount > 0) {
                if (dim == null) dim = new Paint();
                dim.setColor(Color.argb(Math.round(Math.min(1, r.mLp.dimAmount) * 0.6f * 255), 0, 0, 0));
                c.drawRect(0, 0, sw, sh, dim);
            }
            int save = c.save();
            c.translate(r.mX, r.mY);
            c.clipRect(0, 0, r.mW, r.mH);
            if (r.mLp.alpha < 1f) c.saveLayerAlpha(0, 0, r.mW, r.mH, Math.round(r.mLp.alpha * 255));
            drew = true;
            try {
                r.mView.draw(c);
            } catch (RuntimeException e) {
                android.util.Log.e("ViewRoot", "drawing the window threw", e);
            }
            c.restoreToCount(save);
        }
        return drew;
    }

    // ---- input
    /** A touch in screen coordinates, to the window it is for. */
    public static boolean dispatchTouch(MotionEvent e) {
        int am = e.getActionMasked();
        if (am == MotionEvent.ACTION_DOWN || sTouchTarget == null || sTouchTarget.mRemoved) {
            sTouchTarget = null;
            float x = e.getRawX(), y = e.getRawY();
            for (int i = sRoots.size() - 1; i >= 0; i--) {
                ViewRoot r = sRoots.get(i);
                if (r.mView.getVisibility() != View.VISIBLE || (r.mLp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
                boolean inside = x >= r.mX && y >= r.mY && x < r.mX + r.mW && y < r.mY + r.mH;
                boolean modal = (r.mLp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL) == 0;
                if (inside || modal) {
                    if (!inside && (r.mLp.flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
                        MotionEvent o = MotionEvent.obtain(e); o.setAction(MotionEvent.ACTION_OUTSIDE); o.offsetLocation(-r.mX, -r.mY);
                        r.mView.dispatchTouchEvent(o);
                    }
                    sTouchTarget = r;
                    break;
                }
                if ((r.mLp.flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0 && am == MotionEvent.ACTION_DOWN) {
                    MotionEvent o = MotionEvent.obtain(e); o.setAction(MotionEvent.ACTION_OUTSIDE); o.offsetLocation(-r.mX, -r.mY);
                    r.mView.dispatchTouchEvent(o);
                }
            }
            if (am != MotionEvent.ACTION_DOWN && sTouchTarget == null) return false;
        }
        ViewRoot r = sTouchTarget;
        if (r == null) return false;
        MotionEvent l = MotionEvent.obtain(e);
        l.offsetLocation(-r.mX, -r.mY);
        boolean h = r.mView.dispatchTouchEvent(l);
        if (TOUCH_LOG && am != MotionEvent.ACTION_MOVE)
            android.util.Log.d("Touch", MotionEvent.actionToString(am) + " " + l.getX() + "," + l.getY() + " -> window " + r.mView.getClass().getName() + " at " + r.mX + "," + r.mY + " " + r.mW + "x" + r.mH + ": " + (h ? "handled" : "not handled") + ", under it " + deepest(r.mView, l.getX(), l.getY()));
        if (am == MotionEvent.ACTION_UP || am == MotionEvent.ACTION_CANCEL) sTouchTarget = null;
        return h;
    }
    public static boolean dispatchKey(KeyEvent e) {
        for (int i = sRoots.size() - 1; i >= 0; i--) {
            ViewRoot r = sRoots.get(i);
            if (!r.focusable() || r.mView.getVisibility() != View.VISIBLE) continue;
            if (r.mView.dispatchKeyEventPreIme(e)) return true;
            return r.mView.dispatchKeyEvent(e);
        }
        return false;
    }
    public static boolean dispatchGeneric(MotionEvent e) {
        for (int i = sRoots.size() - 1; i >= 0; i--) {
            ViewRoot r = sRoots.get(i);
            if (r.mView.getVisibility() != View.VISIBLE) continue;
            MotionEvent l = MotionEvent.obtain(e);
            l.offsetLocation(-r.mX, -r.mY);
            return r.mView.dispatchGenericMotionEvent(l);
        }
        return false;
    }
    private static android.app.Activity activityOf(android.content.Context c) {
        while (c instanceof android.content.ContextWrapper) {
            if (c instanceof android.app.Activity) return (android.app.Activity) c;
            c = ((android.content.ContextWrapper) c).getBaseContext();
        }
        return null;
    }
    private static final boolean TOUCH_LOG = System.getenv("TL_TOUCH_LOG") != null;
    private static String deepest(View v, float x, float y) {
        String path = v.getClass().getSimpleName();
        while (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            View hit = null;
            for (int i = g.getChildCount() - 1; i >= 0; i--) {
                View c = g.getChildAt(i);
                if (c.getVisibility() != View.VISIBLE) continue;
                float cx = x + g.getScrollX() - c.getLeft(), cy = y + g.getScrollY() - c.getTop();
                if (cx >= 0 && cy >= 0 && cx < c.getWidth() && cy < c.getHeight()) { hit = c; x = cx; y = cy; break; }
            }
            if (hit == null) break;
            v = hit;
            path += " > " + v.getClass().getSimpleName() + (v.isClickable() ? "*" : "");
        }
        return path;
    }

    /** The window touches go to: the top one that takes them (a toast does not). */
    public static ViewRoot topTouchable() {
        for (int i = sRoots.size() - 1; i >= 0; i--) if ((sRoots.get(i).mLp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0) return sRoots.get(i);
        return null;
    }
    public static ViewRoot top() { return sRoots.isEmpty() ? null : sRoots.get(sRoots.size() - 1); }
    /** The focused view of the window keys go to (dispatchKey's), or null: what megler.InputMethods names in the log. */
    public static View keyFocus() {
        for (int i = sRoots.size() - 1; i >= 0; i--) {
            ViewRoot r = sRoots.get(i);
            if (r.focusable() && r.mView.getVisibility() == View.VISIBLE) return r.mFocused;
        }
        return null;
    }

    // ---- ViewParent (the root of a window has no parent view)
    public boolean isLayoutRequested() { return mLayoutRequested; }
    public void requestTransparentRegion(View child) {}
    public void invalidateChild(View child, Rect r) { scheduleFrame(); }
    public ViewParent invalidateChildInParent(int[] loc, Rect r) { scheduleFrame(); return null; }
    public ViewParent getParent() { return null; }
    public void requestChildFocus(View child, View focused) { setFocused(focused); }
    public void recomputeViewAttributes(View child) {}
    public void clearChildFocus(View child) { setFocused(null); }
    public boolean getChildVisibleRect(View child, Rect r, Point offset) { return r.intersect(0, 0, mW, mH); }
    public View focusSearch(View v, int dir) {
        ArrayList<View> all = new ArrayList<>();
        mView.addFocusables(all, dir);
        if (all.isEmpty()) return null;
        int i = all.indexOf(v);
        if (dir == View.FOCUS_FORWARD || dir == View.FOCUS_DOWN || dir == View.FOCUS_RIGHT) return i + 1 < all.size() ? all.get(i + 1) : null;
        return i > 0 ? all.get(i - 1) : null;
    }
    public void bringChildToFront(View child) {}
    public void focusableViewAvailable(View v) {}
    public boolean showContextMenuForChild(View original) { return false; }
    public void createContextMenu(ContextMenu menu) {}
    public ActionMode startActionModeForChild(View original, ActionMode.Callback cb) { return null; }
    public void childDrawableStateChanged(View child) {}
    public void requestDisallowInterceptTouchEvent(boolean d) {}
    public boolean requestChildRectangleOnScreen(View child, Rect r, boolean immediate) { return false; }
    public boolean requestSendAccessibilityEvent(View child, android.view.accessibility.AccessibilityEvent e) { return false; }
    public void childHasTransientStateChanged(View child, boolean has) {}
    public void requestFitSystemWindows() { requestApplyInsets(); }
    public ViewParent getParentForAccessibility() { return null; }
    public void notifySubtreeAccessibilityStateChanged(View child, View source, int type) {}
    public boolean canResolveLayoutDirection() { return true; }
    public boolean isLayoutDirectionResolved() { return true; }
    public int getLayoutDirection() { return View.LAYOUT_DIRECTION_LTR; }
    public boolean canResolveTextDirection() { return true; }
    public boolean isTextDirectionResolved() { return true; }
    public int getTextDirection() { return View.TEXT_DIRECTION_FIRST_STRONG; }
    public boolean canResolveTextAlignment() { return true; }
    public boolean isTextAlignmentResolved() { return true; }
    public int getTextAlignment() { return View.TEXT_ALIGNMENT_GRAVITY; }
    public boolean onStartNestedScroll(View child, View target, int axes) { return false; }
    public void onNestedScrollAccepted(View child, View target, int axes) {}
    public void onStopNestedScroll(View target) {}
    public void onNestedScroll(View target, int dxc, int dyc, int dxu, int dyu) {}
    public void onNestedPreScroll(View target, int dx, int dy, int[] consumed) {}
    public boolean onNestedFling(View target, float vx, float vy, boolean consumed) { return false; }
    public boolean onNestedPreFling(View target, float vx, float vy) { return false; }
    public boolean onNestedPrePerformAccessibilityAction(View target, int action, android.os.Bundle args) { return false; }
}
