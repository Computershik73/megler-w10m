// Overlay of core/framework/src/megler/ViewRoot.java (tools/build-framework.sh).
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
// And in the frame the window's insets give (computeInsets; WindowInsets.getFrame, overlaid): the screen's size for an
// activity's window, a dialog's or popup's own size once laid out.
//
// And in the size the screen's canvas says (ScreenCanvas): the window's while its views draw, and each view's own while it
// draws (View.drawFromParent, overlaid), as Android's canvas of a view is a display list of the view's size; the original's
// canvas said the screen's everywhere.
//
// And in keyFocus(), which the original does not have: the focused view of the window keys go to, which the overlay's
// megler.InputMethods names in the log when typed text arrives.
//
// And it no longer tells android.webkit.WebView that windows came and went (meglerWindowsChanged, which the original's
// WebView over WKWebView used to place its host's view): a WebView is Android's here, Chromium, drawn as a view.
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
// And in the phone's bars (updateBars, at every frame): which of them the top activity window wants hidden -- its
// FLAG_FULLSCREEN, its views' system UI visibility, what its WindowInsetsController hid -- is told to the shell
// (Native.systemBars), which hides the phone's status and navigation bars accordingly and says the insets that follow; the
// controller's hide and show of the bars are kept by the decor (megler.DecorView, overlaid). The original's controller did
// the keyboard alone, and the shell kept every app in full screen whatever it asked.
//
// Why. Where a frame's time goes -- in Java and in which part of the frame, in the shell's drawing, in handing the picture
// over -- is measured from the core's side (core/src/drivers/mg_java.c, "the frame meter", a line in the log every five
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
    /* A window of an activity's other than its own (a dialog, a popup) while that activity is stopped: hidden, as Android's
       window manager hides the windows of a token that is not visible (ownerStopped). */
    private boolean mOwnerStopped;
    /** What of this window is to be drawn again, in its own coordinates; or all of it. */
    private final Rect mDirty = new Rect();
    private boolean mDirtyAll = true;
    /** The whole screen is to be drawn at the next frame: a window came, went, moved or changed its size. */
    private static boolean sFullRedraw = true, sDrawingPart;
    /* Why the next frame draws the whole screen (fullRedraw): what the log's MeglerViews line names. */
    private static String sFullWhy;
    private static void fullRedraw(String why) { sFullRedraw = true; if (sFullWhy == null) sFullWhy = why; }
    private static int sMovesSaid;
    private String windowName() { return mView.getClass().getName() + (mLp.type >= 1000 ? " (window type " + mLp.type + ")" : ""); }
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
        // A dialog of an activity that is stopped comes up hidden, and shows when the activity is on the screen again.
        android.app.Activity of = r.ofToken() ? activityOf(v.getContext()) : null;
        if (of != null && of.meglerWindowsHidden()) { r.mOwnerStopped = true; r.mHasFocus = false; }
        sRoots.add(at, r);
        if (under) for (int i = at + 1; i < sRoots.size(); i++) if (sRoots.get(i).focusable() && !sRoots.get(i).mOwnerStopped) { r.mHasFocus = false; break; }
        if (v.getLayoutParams() == null) v.setLayoutParams(r.mLp);
        fullRedraw("a window added: " + r.windowName());
        r.attach();
        if (r.mHasFocus) for (ViewRoot o : sRoots) if (o != r && o.mHasFocus && r.focusable()) { o.mHasFocus = false; o.mView.dispatchWindowFocusChanged(false); o.mObserver.meglerWindowFocus(false); }
        scheduleFrame();
        return r;
    }
    private WindowInsets computeInsets() {
        boolean full = mLp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION || mLp.type == WindowManager.LayoutParams.TYPE_APPLICATION;
        mAdjust = adjustMode();
        boolean resize = mAdjust == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
        /* A dialog lies above the keyboard when it is resized and is moved, not shrunk, when it is panned: no part of it is an inset. */
        WindowInsets i;
        if (floatingAppWindow()) i = WindowInsets.meglerOf(0, 0, 0, 0, 0, null);
        else if (full) i = WindowInsets.meglerOf(sInsets[0], sInsets[1], sInsets[2], sInsets[3], resize ? sImeBottom : 0, cutout());
        else i = WindowInsets.meglerOf(0, 0, 0, 0, Math.max(0, sImeBottom - (screenH() - mY - mH)), null);
        /* The frame they are relative to (WindowInsets.getFrame): the screen for an activity's window, which fills it; another
           window's own size once it has been laid out, the screen's before. */
        return full || mW <= 0 || mH <= 0 ? i.meglerFrame(screenW(), screenH()) : i.meglerFrame(mW, mH);
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
            fullRedraw("a window panned for the keyboard: " + windowName());
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
        fullRedraw("a window removed: " + windowName());
        boolean hadFocus = mHasFocus;
        sRoots.remove(this);
        if (sTouchTarget == this) sTouchTarget = null;
        mView.meglerDetach();
        mObserver.meglerAttached(false);
        if (hadFocus) {
            for (int i = sRoots.size() - 1; i >= 0; i--) {
                ViewRoot o = sRoots.get(i);
                if (o.focusable() && !o.mOwnerStopped) { if (!o.mHasFocus) { o.mHasFocus = true; o.mView.dispatchWindowFocusChanged(true); o.mObserver.meglerWindowFocus(true); } break; }
            }
        }
        scheduleFrame();
    }
    /*
     * WindowManager.updateViewLayout. The whole screen is drawn again only for what changes how the window looks apart from
     * where it is and its size, which the next layout compares (performLayout): Yandex Maps updated a window's parameters in
     * nearly every frame, each a whole screen drawn again (package 530).
     */
    public void setLayoutParams(WindowManager.LayoutParams lp) {
        boolean looks = lp.type != mLp.type || lp.flags != mLp.flags || lp.format != mLp.format || lp.alpha != mLp.alpha || lp.dimAmount != mLp.dimAmount;
        mLp.copyFrom(lp);
        mView.setLayoutParams(mLp);
        mLayoutRequested = true;
        if (looks) fullRedraw("a window's parameters changed: " + windowName());
        scheduleFrame();
    }

    /*
     * The windows an activity opened besides its own -- its dialogs and popups -- hidden while it is stopped and shown again
     * when it is on the screen (Activity.meglerStop, makeVisible), as Android hides a token's windows: they are not drawn and
     * take no touches, keys or focus, and stay attached. They were drawn all the while: WhatsApp's progress dialog of its
     * phone number screen showed through the screens after it.
     */
    public static void ownerStopped(android.app.Activity a, boolean stopped) {
        boolean any = false;
        for (ViewRoot r : sRoots) {
            if (r.mRemoved || r.mOwnerStopped == stopped || !r.ofToken() || activityOf(r.mView.getContext()) != a) continue;
            r.mOwnerStopped = stopped;
            r.mView.dispatchWindowVisibilityChanged(stopped ? View.GONE : View.VISIBLE);
            any = true;
        }
        if (!any) return;
        if (sTouchTarget != null && sTouchTarget.mOwnerStopped) sTouchTarget = null;
        refocus();
        fullRedraw("an activity's windows hidden or shown again");
        scheduleFrame();
    }

    /*
     * The windows of an activity that finished and has not stopped yet (AppRunner.finishNow), its own among them, hidden as
     * the one below comes back, as Android hides a finishing activity's windows then: not drawn, and given no touches, keys
     * or focus, and still attached until the activity stops.
     */
    public static void finishedHidden(android.app.Activity a) {
        boolean any = false;
        for (ViewRoot r : sRoots) {
            if (r.mRemoved || r.mOwnerStopped || r.mLp.type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW || activityOf(r.mView.getContext()) != a) continue;
            r.mOwnerStopped = true;
            r.mView.dispatchWindowVisibilityChanged(View.GONE);
            any = true;
        }
        if (!any) return;
        if (sTouchTarget != null && sTouchTarget.mOwnerStopped) sTouchTarget = null;
        refocus();
        fullRedraw("a finished activity's windows hidden");
        scheduleFrame();
    }

    /*
     * The windows an activity that has been destroyed still had besides its own, removed, as Android's
     * WindowManagerGlobal.closeAll removes them ("has leaked window"); how many. A dialog an activity left showing as it
     * finished stayed in the list of windows, drawn with whatever came after it: WhatsApp's progress dialog of its phone
     * number screen.
     */
    public static int closeAll(android.app.Activity a) {
        int n = 0;
        for (ViewRoot r : new ArrayList<>(sRoots)) {
            if (r.mRemoved || !r.ofToken() || activityOf(r.mView.getContext()) != a) continue;
            android.util.Log.w("Megler", "the activity " + a.getClass().getName() + " ended with a window of its still there (Android: it has leaked window "
                               + r.mView.getClass().getName() + "): removed");
            r.remove();
            n++;
        }
        return n;
    }

    /* One of an activity's windows besides its own: a dialog (an application window) or a panel or popup (a sub-window), which
       go with the activity's token on Android -- not a system window such as a toast, which outlives the activity. */
    private boolean ofToken() { return mLp.type > WindowManager.LayoutParams.TYPE_BASE_APPLICATION && mLp.type < WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW; }

    /* The window focus where Android's window manager puts it: the top window that can take it and is not hidden. */
    private static void refocus() {
        ViewRoot top = null;
        for (int i = sRoots.size() - 1; i >= 0 && top == null; i--) {
            ViewRoot r = sRoots.get(i);
            if (!r.mRemoved && !r.mOwnerStopped && r.focusable()) top = r;
        }
        for (ViewRoot r : sRoots) {
            boolean want = r == top;
            if (r.mHasFocus == want) continue;
            r.mHasFocus = want;
            r.mView.dispatchWindowFocusChanged(want);
            r.mObserver.meglerWindowFocus(want);
        }
    }
    public WindowManager.LayoutParams layoutParams() { return mLp; }
    private boolean focusable() { return (mLp.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0; }

    // ---- what views ask of their root
    public ViewTreeObserver observer() { return mObserver; }
    public Handler handler() { return mainHandler(); }
    public android.os.IBinder token() { return mToken; }
    /*
     * The window's WindowId (View.getWindowId): one for the window, so that two views of it have equal ones, as Android's
     * equal by the window's token. Null was what every view had, and Spotify's main layout, comparing its now-playing view's
     * with its own (getWindowId().equals), ended the app at its start (package 551).
     */
    private android.view.WindowId mWindowId;
    public android.view.WindowId windowId() {
        if (mWindowId == null) mWindowId = new android.view.WindowId(mToken);
        return mWindowId;
    }
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
    public void invalidate() { mDirtyAll = true; noteDirtier("the window", Float.MAX_VALUE); scheduleFrame(); }
    /** The whole window, for a view of it (one with an animation running: View.meglerDirty). */
    public void invalidate(View cause) { mDirtyAll = true; noteDirtier(cause.getClass().getName(), Float.MAX_VALUE); scheduleFrame(); }
    public void invalidateFromAnyThread() { if (Looper.myLooper() == Looper.getMainLooper()) invalidate(); else mainHandler().post(this::invalidate); }
    /** A rectangle of the window, in its own coordinates (View.meglerDirty), is to be drawn again; nothing when none of it is in the window. */
    public void meglerDirty(RectF box, View cause) {
        if (!mDirtyAll) {
            int l = Math.max(0, (int) Math.floor(box.left)), t = Math.max(0, (int) Math.floor(box.top));
            int r = Math.min(mW, (int) Math.ceil(box.right)), b = Math.min(mH, (int) Math.ceil(box.bottom));
            if (l >= r || t >= b) return;
            mDirty.union(l, t, r, b);
            noteDirtier(cause, (float) (r - l) * (b - t));
        }
        scheduleFrame();
    }
    /*
     * Which views have most of the screen drawn again: in each frame that draws half the screen or more, the view (by its
     * class) that asked for the largest rectangle, counted, and the three counted most said every ten seconds
     * (I/MeglerViews). Yandex Maps redrew the whole screen in every frame, and nothing said for which view.
     */
    private static String sDirtier;
    private static View sDirtierView;
    private static float sDirtierArea;
    private static final java.util.HashMap<String, int[]> sDirtiers = new java.util.HashMap<>();
    private static long sDirtiersSaid;
    private static void noteDirtier(View cause, float area) {
        if (cause != null && area > sDirtierArea) { sDirtierView = cause; sDirtier = null; sDirtierArea = area; }
    }
    private static void noteDirtier(String cause, float area) {
        if (area > sDirtierArea) { sDirtier = cause; sDirtierView = null; sDirtierArea = area; }
    }
    private static void countDirtier(int redrawn, int screen) {
        String who = sDirtierView != null ? sDirtierView.getClass().getName() : sDirtier;
        sDirtier = null;
        sDirtierView = null;
        sDirtierArea = 0;
        if (who != null && redrawn * 2L >= screen) {
            int[] n = sDirtiers.get(who);
            if (n == null) sDirtiers.put(who, n = new int[1]);
            n[0]++;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (now - sDirtiersSaid < 10000 || sDirtiers.isEmpty()) return;
        sDirtiersSaid = now;
        StringBuilder b = new StringBuilder("frames that drew half the screen or more, in 10 s, by the view that asked for the most:");
        for (int k = 0; k < 3 && !sDirtiers.isEmpty(); k++) {
            String best = null;
            int most = 0;
            for (java.util.Map.Entry<String, int[]> e : sDirtiers.entrySet()) if (e.getValue()[0] > most) { most = e.getValue()[0]; best = e.getKey(); }
            b.append(k == 0 ? " " : ", ").append(best).append(' ').append(most);
            sDirtiers.remove(best);
        }
        sDirtiers.clear();
        android.util.Log.i("MeglerViews", b.toString());
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
            public void show(int types) {
                if ((types & WindowInsets.Type.ime()) != 0 && mFocused != null) InputMethods.show(mFocused);
                if (mView instanceof DecorView) ((DecorView) mView).meglerHideBars(types, false);
            }
            public void hide(int types) {
                if ((types & WindowInsets.Type.ime()) != 0) InputMethods.hide();
                if (mView instanceof DecorView) ((DecorView) mView).meglerHideBars(types, true);
            }
            public void setSystemBarsAppearance(int a, int mask) {
                appearance = (appearance & ~mask) | (a & mask);
                if (mView instanceof DecorView) ((DecorView) mView).meglerSetAppearance(a, mask);
            }
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
    /* The phone has no cutout: what covers the top of the screen is its status bar, an inset like any other. (The original's
       shell, iOS's, had a notch there, which reached an app as a cutout and outlived the decor's consuming the insets: an app
       that keeps its views off a cutout itself, as Instagram does, then kept off the status bar twice.) */
    public static DisplayCutout cutout() { return null; }
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
        updateBars(roots);
        for (ViewRoot r : roots) if (!r.mRemoved && r.mObserver.dispatchOnPreDraw()) { scheduleFrame(); }
        Native.frameMark(2, 0);
        drawAll(roots);
        for (ViewRoot r : roots) if (!r.mRemoved) r.mObserver.dispatchOnDraw();
        Native.frameMark(3, sRedrawn);
    }
    /**
     * The phone's bars the top activity window wants hidden, and whether its status bar is light, told to the shell when
     * that changes (Native.systemBars: 1 the status bar, 2 the navigation bar, 4 light): the window's FLAG_FULLSCREEN, the
     * system UI visibility its views set (SYSTEM_UI_FLAG_FULLSCREEN, _HIDE_NAVIGATION, _LIGHT_STATUS_BAR), and what its
     * WindowInsetsController hid and its appearance (kept by the decor). Android's status bar goes for the first and the
     * navigation bar for the second, and the window's insets follow; here the shell hides the phone's own bars and says the
     * insets that follow (Native.insets). Asked at every frame after layout: a flag set, a controller asked and a window
     * that comes or goes all bring one.
     */
    private static int sBars = -1;
    private static void updateBars(ArrayList<ViewRoot> roots) {
        ViewRoot top = null;
        for (int i = roots.size() - 1; i >= 0 && top == null; i--) {
            ViewRoot r = roots.get(i);
            if (!r.mRemoved && r.mLp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION && r.mView instanceof DecorView) top = r;
        }
        if (top == null) return;
        DecorView decor = (DecorView) top.mView;
        Window w = decor.getWindowMegler();
        int sys = top.mSystemUi, hidden = decor.meglerHiddenBars(), bars = 0;
        if ((w != null && w.meglerFullscreen()) || (sys & View.SYSTEM_UI_FLAG_FULLSCREEN) != 0 || (hidden & WindowInsets.Type.statusBars()) != 0) bars |= 1;
        if ((sys & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) != 0 || (hidden & WindowInsets.Type.navigationBars()) != 0) bars |= 2;
        if ((sys & View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR) != 0 || (decor.meglerAppearance() & WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS) != 0) bars |= 4;
        if (bars == sBars) return;
        sBars = bars;
        Native.systemBars(bars);
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
        if (v.getPaddingLeft() != 0 || v.getPaddingTop() != 0 || v.getPaddingRight() != 0 || v.getPaddingBottom() != 0)
            b.append(" pad=").append(v.getPaddingLeft()).append(',').append(v.getPaddingTop()).append(',').append(v.getPaddingRight()).append(',').append(v.getPaddingBottom());
        if (v.getTranslationY() != 0f) b.append(" ty=").append(v.getTranslationY());
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
        if (v instanceof android.widget.ImageView) {
            // its picture: the drawable, where it is drawn, how, and in what colour (Organic Maps' map buttons drew none)
            android.widget.ImageView iv = (android.widget.ImageView) v;
            android.graphics.drawable.Drawable d = iv.getDrawable();
            if (d == null) b.append(" img=none");
            else {
                b.append(" img=").append(d.getClass().getSimpleName()).append(' ').append(d.getIntrinsicWidth()).append('x').append(d.getIntrinsicHeight())
                 .append(" bounds=").append(d.getBounds().toShortString()).append(" scale=").append(iv.getScaleType()).append(" a=").append(d.getAlpha());
                if (!iv.getImageMatrix().isIdentity()) b.append(" m=").append(iv.getImageMatrix().toShortString());
                if (iv.getImageTintList() != null) b.append(" itint=").append(Integer.toHexString(iv.getImageTintList().getDefaultColor()));
                if (d.getColorFilter() != null) b.append(" cf=").append(d.getColorFilter().getClass().getSimpleName());
                if (d instanceof android.graphics.drawable.VectorDrawable) b.append(" [").append(((android.graphics.drawable.VectorDrawable) d).meglerDescribe()).append(']');
            }
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
            // a ConstraintLayout's constraints: the fields the class has, looked up once each (a getField that misses
            // throws, and with trace-exceptions on that filled its budget of lines with nothing)
            for (java.lang.reflect.Field fl : lp.getClass().getFields()) {
                String f = fl.getName();
                if (!"topToTop".equals(f) && !"topToBottom".equals(f) && !"bottomToBottom".equals(f) && !"leftToLeft".equals(f) && !"startToStart".equals(f)
                    && !"endToEnd".equals(f) && !"rightToRight".equals(f) && !"horizontalBias".equals(f) && !"dimensionRatio".equals(f)) continue;
                try { Object o = fl.get(lp); if (o != null && !"-1".equals(String.valueOf(o)) && !"0.5".equals(String.valueOf(o))) b.append(' ').append(f).append('=').append(o); } catch (Exception e) {}
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
            if (out.left != mX || out.top != mLaidY || w != mW || h != mH) {
                fullRedraw("a window moved or resized: " + windowName());
                if (sMovesSaid++ < 10)
                    android.util.Log.i("MeglerViews", "a window moved or resized: " + windowName() + " " + mW + "x" + mH + " at " + mX + "," + mLaidY + " to " + w
                                       + "x" + h + " at " + out.left + "," + out.top);
            }
            mX = out.left; mLaidY = out.top; mY = mLaidY - mPan; mW = w; mH = h;
            mView.layout(0, 0, w, h);
        } finally { mInLayout = false; }
        mObserver.dispatchOnGlobalLayout();
        updatePan();
        /* (On the phone, the flag trace-views: a file of that name in the data folder's flags.) */
        if (sDump == null) sDump = System.getenv("TL_VIEW_DUMP") != null || Native.flag("trace-views");
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
            // (Not while the render thread draws the frame before onto the old one.)
            Gfx.frameWait();
            if (sCanvas != null) sCanvas.meglerRelease();
            sScreen = Bitmap.createBitmap(sw, sh, Bitmap.Config.ARGB_8888);
            sCanvas = new Canvas(sScreen);
            whole = true;
        }
        if (sFrame == null) sFrame = new ScreenCanvas();
        // What the windows asked for, on the screen
        Rect part = sPart;
        part.setEmpty();
        for (ViewRoot r : roots) {
            if (r.mRemoved || r.mOwnerStopped) continue;
            if (r.mDirtyAll) whole = true;
            else if (!r.mDirty.isEmpty()) { sScratch.set(r.mDirty); sScratch.offset(r.mX, r.mY); part.union(sScratch); }
        }
        for (ViewRoot r : roots) { r.mDirty.setEmpty(); r.mDirtyAll = false; }
        // The views' display lists that went invalid under valid ones, recorded again before anything is drawn (View, overlaid).
        // A view whose drawing throws is left as far as it got, as when the window is drawn (drawWindows): Telegram's chat
        // list cell throws in its onDraw on every frame here, and the app ended on it once its lists were recorded before
        // the frame (package 544), where it had gone on before.
        for (ViewRoot r : roots) {
            if (r.mRemoved || r.mOwnerStopped) continue;
            try {
                r.mView.meglerUpdateLists();
            } catch (RuntimeException e) {
                android.util.Log.e("ViewRoot", "recording the window's display lists threw", e);
            }
        }
        if (sFullRedraw) noteDirtier(sFullWhy != null ? sFullWhy : "windows added, removed, moved or resized", Float.MAX_VALUE);
        sFullWhy = null;
        sFullRedraw = false;
        countDirtier(whole ? sw * sh : (part.isEmpty() ? 0 : Math.min(part.width(), sw) * Math.min(part.height(), sh)), sw * sh);
        // The frame is recorded (sFrame), and the render thread draws it onto the screen and shows it (Gfx.frame) while this
        // thread goes on: the views' lists are drawn there by reference, as Android's RenderThread draws them.
        if (!whole) {
            sRedrawn = 0;
            if (!sPresentedAny || !part.intersect(0, 0, sw, sh)) return;      /* nothing visible changed: the screen is as it was */
            sFrame.meglerFrame(sw, sh);
            int save = sFrame.save();
            sFrame.clipRect(part);
            sFrame.drawColor(0, PorterDuff.Mode.CLEAR);
            sDrawingPart = true;
            try { drawWindows(sFrame, roots, sw, sh); } finally { sDrawingPart = false; sFrame.restoreToCount(save); }
            sRedrawn = part.width() * part.height();
            if (sCheck == 0) sCheck = Native.checkPartial() ? 1 : -1;
            handOver();
            Gfx.frame(sCanvas.meglerNative(), sScreen.meglerNative(), sFrame.meglerNative(), part.left, part.top, part.right, part.bottom);
            if (sCheck > 0) { Gfx.frameWait(); checkPart(roots, sw, sh, part); }
            return;
        }
        sFrame.meglerFrame(sw, sh);
        sFrame.drawColor(0, PorterDuff.Mode.CLEAR);
        // a window drawn counts even when it drew nothing (the screen must lose a closed dialog)
        boolean any = drawWindows(sFrame, roots, sw, sh);
        sRedrawn = any ? sw * sh : 0;
        // With no window drawn the screen is cleared all the same (drawn, not shown): a frame drawn in part later keeps the rest.
        handOver();
        Gfx.frame(sCanvas.meglerNative(), sScreen.meglerNative(), sFrame.meglerNative(), 0, 0, any ? 0 : -1, 0);
        if (!any && sPresentedAny) Gfx.present(0);
        sPresentedAny = any;
    }
    /* The canvas each frame is recorded on, for the render thread. */
    private static ScreenCanvas sFrame;
    /*
     * Before a frame is handed over: the frame before shown, then the nodes' properties that changed handed to the core, which
     * drawing on the screen did before (RenderNode.meglerSync) and recording does not, so that the render thread draws each
     * frame with its own.
     */
    private static void handOver() {
        Gfx.frameWait();
        android.graphics.RenderNode.meglerSyncAll();
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
            sWholeCanvas = new ScreenCanvas(sWhole);
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
    /**
     * The screen's canvas. While a window's views draw on it, its width and height are the window's, and each view's own while
     * that view draws (View.drawFromParent): on Android a view draws into a display list of its own size, and an onDraw that
     * reads canvas.getWidth() means the view's width. Anything else of the canvas is the screen's, as before.
     */
    public static final class ScreenCanvas extends Canvas {
        private int mViewW = -1, mViewH = -1, mScreenW = -1, mScreenH = -1;

        ScreenCanvas(Bitmap b) { super(b); }
        /* One that records the window's frames (drawAll), of the screen's size. */
        ScreenCanvas() { super(); }

        /** A frame of the screen's size recorded from now on (megler.Gfx.frame hands it over). */
        void meglerFrame(int w, int h) {
            meglerRecordFrame(w, h);
            mScreenW = w;
            mScreenH = h;
        }

        @Override public int getWidth() { return mViewW >= 0 ? mViewW : mScreenW >= 0 ? mScreenW : super.getWidth(); }
        @Override public int getHeight() { return mViewH >= 0 ? mViewH : mScreenH >= 0 ? mScreenH : super.getHeight(); }

        /** The size said from now on; what was said before, for meglerViewSizeBack. */
        public long meglerViewSize(int w, int h) {
            long was = ((long) mViewW << 32) | (mViewH & 0xFFFFFFFFL);
            mViewW = w;
            mViewH = h;
            return was;
        }

        public void meglerViewSizeBack(long was) {
            mViewW = (int) (was >> 32);
            mViewH = (int) was;
        }
    }

    /** Every window, bottom first, into a canvas of the screen as it is clipped; whether any was drawn. */
    private static boolean drawWindows(Canvas c, ArrayList<ViewRoot> roots, int sw, int sh) {
        Paint dim = null;
        boolean drew = false;
        for (ViewRoot r : roots) {
            if (r.mRemoved || r.mOwnerStopped || r.mView.getVisibility() != View.VISIBLE) continue;
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
            ScreenCanvas screen = c instanceof ScreenCanvas ? (ScreenCanvas) c : null;
            long was = screen != null ? screen.meglerViewSize(r.mW, r.mH) : 0;
            try {
                r.mView.draw(c);
            } catch (RuntimeException e) {
                android.util.Log.e("ViewRoot", "drawing the window threw", e);
            } finally {
                if (screen != null) screen.meglerViewSizeBack(was);
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
                if (r.mOwnerStopped || r.mView.getVisibility() != View.VISIBLE || (r.mLp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
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
            if (!r.focusable() || r.mOwnerStopped || r.mView.getVisibility() != View.VISIBLE) continue;
            if (r.mView.dispatchKeyEventPreIme(e)) return true;
            return r.mView.dispatchKeyEvent(e);
        }
        return false;
    }
    public static boolean dispatchGeneric(MotionEvent e) {
        for (int i = sRoots.size() - 1; i >= 0; i--) {
            ViewRoot r = sRoots.get(i);
            if (r.mOwnerStopped || r.mView.getVisibility() != View.VISIBLE) continue;
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
        for (int i = sRoots.size() - 1; i >= 0; i--) if ((sRoots.get(i).mLp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0 && !sRoots.get(i).mOwnerStopped) return sRoots.get(i);
        return null;
    }
    public static ViewRoot top() { return sRoots.isEmpty() ? null : sRoots.get(sRoots.size() - 1); }
    /** The focused view of the window keys go to (dispatchKey's), or null: what megler.InputMethods names in the log. */
    public static View keyFocus() {
        for (int i = sRoots.size() - 1; i >= 0; i--) {
            ViewRoot r = sRoots.get(i);
            if (r.focusable() && !r.mOwnerStopped && r.mView.getVisibility() == View.VISIBLE) return r.mFocused;
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
