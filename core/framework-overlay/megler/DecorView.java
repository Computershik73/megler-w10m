// Overlay of core/framework/src/megler/DecorView.java (tools/build-framework.sh).
// It differs in dispatchApplyWindowInsets: the content stack's margins are set only while the stack is still the decor's
// own child with the decor's (FrameLayout) layout parameters. An app may take the decor's content out and put it in a view
// of its own, and that view in the decor -- the old way to put a navigation drawer over the whole window, the action bar
// included, which VK mp3 mod does with a DrawerLayout. Android's decor then leaves that content alone. The original cast
// the stack's parameters to FrameLayout.LayoutParams whatever they were, and VK ended at its first layout after its sign-in
// with "DrawerLayout$LayoutParams cannot be cast to FrameLayout$LayoutParams" (package 271).
//
// And in the window's WindowInsetsController: the status and navigation bars it hides and shows, and the bars' appearance,
// are kept here (meglerHiddenBars, meglerAppearance), by the controller of the attached window (megler.ViewRoot's) and by one of
// the decor's own before that (Android's PendingInsetsController), so that what an app asks in onCreate counts; megler.ViewRoot
// tells the shell which bars to hide. The original's controller before attach did nothing.
//
// And the insets it gives its content when it keeps the content off both bars still reach every view of it, consumed
// (WindowInsets.meglerToChildren), as they did before consumed insets ended their dispatch (WindowInsets, overlaid): the
// content's own listeners are told of the window's insets with nothing left in them.
//
// And in what the content frame is told of the window's height (makeContent): in a window without a title bar the frame
// fills what the stack has left, with no weight, as the content of Android's screen_simple layout does. The original gave
// it height 0 and weight 1 in every window. That is the same in a window of a set size; in one that wraps its content, a
// dialog, LinearLayout measures a weighted child twice: as tall as it likes within the screen, and then again at exactly
// the height it answered. Android's dialogs without a window title (every AlertDialog, and an app's own with
// windowNoTitle) are never told "exactly": their content hears "at most the screen" and nothing else. Telegram's own
// AlertDialog counts on that. Its container measures the title's frame and the row of buttons with the height it was
// itself given, so told "exactly 337" it made each of the two 337 pixels high, itself 787, and the message and the
// buttons lay below the window's 337: "Log out?" was a title on an empty card that could not be answered.
//
// And in the theme's windowBackgroundFallback (draw, backgroundFallback): a window without a background has it drawn where
// its content does not reach -- the status bar's strip the content is kept off, the navigation bar's -- as Android's
// DecorView does (BackgroundFallback); the framework's themes make it ?attr/colorBackground. YouTube takes its window's
// background away once it has started and makes the status bar's colour transparent, and its status bar was black here
// (with the dark icons of a light status bar on it), where Android shows its page's colour.
//
// And in a window whose input is taken (Window.takeInputQueue, NativeActivity's): its touches and keys go into the queue
// native code reads (Window.meglerTakenInput) instead of to the callback and the views, and the queue is made when the
// window is added and goes when it is removed (Window.meglerAttached, meglerDetached), as ViewRootImpl has it. An app on
// top of NativeActivity, run by the Java driver since issue #23 (KOReader), reads all its input so.
//
// Everything else is the original's, unchanged.
package megler;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.*;
import android.graphics.drawable.Drawable;
import android.util.TypedValue;
import android.view.*;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * A window's decor: the window background, the status and navigation bar areas (painted in their colours unless the app draws there
 * itself), an action bar when the theme has one, and the content frame (android.R.id.content). Its events go to the window's callback.
 */
public final class DecorView extends FrameLayout {
    private final Window mWindow;
    private Drawable mBackground;
    private LinearLayout mStack;
    private FrameLayout mContent;
    private android.widget.Toolbar mTitle;
    // (The title bar of the themes before Holo: its frame, and its text unless the title is the app's own.)
    private FrameLayout mTitleFrame;
    private TextView mPlainTitle;
    private View mActionBar;
    private boolean mCloseOnTouchOutside;
    private int mInsetTop, mInsetBottom, mInsetLeft, mInsetRight;
    private final Paint mBarPaint = new Paint();
    private static WindowInsetsController sFallback;
    private TypedValue mMinWidthMajor, mMinWidthMinor;
    private static TypedValue copy(TypedValue v) { if (v == null) return null; TypedValue c = new TypedValue(); c.type = v.type; c.data = v.data; return c; }

    public DecorView(Context c, Window w) {
        super(c);
        mWindow = w;
        setWillNotDraw(false);
        TypedArray a = c.obtainStyledAttributes(S.Window);
        mCloseOnTouchOutside = a.getBoolean(S.Window_windowCloseOnTouchOutside, false);
        mMinWidthMajor = copy(a.peekValue(S.Window_windowMinWidthMajor));
        mMinWidthMinor = copy(a.peekValue(S.Window_windowMinWidthMinor));
        a.recycle();
    }
    public static WindowInsetsController fallbackInsetsController() {
        if (sFallback == null) sFallback = new WindowInsetsController() {
            public void show(int t) {} public void hide(int t) {} public void setSystemBarsAppearance(int a, int m) {} public int getSystemBarsAppearance() { return 0; }
            public void setSystemBarsBehavior(int b) {} public int getSystemBarsBehavior() { return 0; }
        };
        return sFallback;
    }
    /** The window's controller, before the decor is attached too (Android's PendingInsetsController): what it hides is kept here. */
    @Override public WindowInsetsController getWindowInsetsController() {
        WindowInsetsController c = super.getWindowInsetsController();
        if (c != null) return c;
        if (mPending == null) mPending = new WindowInsetsController() {
            int behavior;
            public void show(int t) { meglerHideBars(t, false); } public void hide(int t) { meglerHideBars(t, true); }
            public void setSystemBarsAppearance(int a, int m) { meglerSetAppearance(a, m); } public int getSystemBarsAppearance() { return mAppearance; }
            public void setSystemBarsBehavior(int b) { behavior = b; } public int getSystemBarsBehavior() { return behavior; }
        };
        return mPending;
    }
    /*
     * The phone's bars as the window's WindowInsetsController asks for them: the status and navigation bars it hid
     * (WindowInsets.Type bits) and its bars' appearance (APPEARANCE_LIGHT_STATUS_BARS). Kept by the decor, which is there
     * before the window is attached; megler.ViewRoot tells the shell (Native.systemBars) at its next frame.
     */
    private int mHiddenBars, mAppearance;
    private WindowInsetsController mPending;
    public int meglerHiddenBars() { return mHiddenBars; }
    public int meglerAppearance() { return mAppearance; }
    public void meglerHideBars(int types, boolean hide) {
        int bars = types & (WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
        if (bars == 0) return;
        mHiddenBars = hide ? mHiddenBars | bars : mHiddenBars & ~bars;
        requestApplyInsets();
    }
    public void meglerSetAppearance(int a, int mask) { mAppearance = (mAppearance & ~mask) | (a & mask); requestApplyInsets(); }
    public void setCloseOnTouchOutside(boolean c) { mCloseOnTouchOutside = c; }
    public boolean closeOnTouchOutside() { return mCloseOnTouchOutside; }
    /** The window background; its padding (a Material dialog's InsetDrawable: the card inset from the window's edges) keeps the
     *  content inside it, as View.setBackground does on Android. */
    public void setWindowBackground(Drawable d) {
        /* (The same drawable again: nothing, as View.setBackground on Android; one that changes says so itself, invalidateDrawable.) */
        if (d == mBackground) return;
        mBackground = d;
        Rect p = new Rect();
        if (d != null) { d.setCallback(this); if (!d.getPadding(p)) p.setEmpty(); }
        setPadding(p.left, p.top, p.right, p.bottom);
        invalidate();
        requestLayout();
    }
    @Override protected boolean verifyDrawable(Drawable who) { return who == mBackground || super.verifyDrawable(who); }
    /*
     * The whole window asked to be drawn again: the 50th time, the 100th, 200th, 400th and 800th said with who asked, in the
     * log (MeglerViews) -- past the window's setting up, which asks a few times -- since a decor that asks so in every frame
     * has the whole screen drawn in every frame.
     */
    private static int sWholeAsked;
    @Override public void invalidate() {
        int n = ++sWholeAsked;
        if (n == 50 || n == 100 || n == 200 || n == 400 || n == 800)
            android.util.Log.i("MeglerViews", "the whole window asked to be drawn again, the " + n + "th time, by", new Throwable());
        super.invalidate();
    }
    /** A bar's colour changed (Window.setStatusBarColor, setNavigationBarColor): the strip draw paints it over, when there is one. */
    public void meglerBarChanged(boolean status) {
        if (status) {
            if (mInsetTop > 0) invalidate(0, 0, getWidth(), mInsetTop);
        } else if (mInsetBottom > 0) {
            invalidate(0, getHeight() - mInsetBottom, getWidth(), getHeight());
        }
    }

    /** The views between the decor and the content: an action bar when asked for, then the content frame. */
    public FrameLayout makeContent(boolean actionBar, CharSequence title) { return makeContent(actionBar, false, false, title); }

    /**
     * The same, or with the title bar of the themes before Holo (titleBar: a window with neither windowNoTitle nor
     * windowActionBar), as Android's screen_title layout: the title (android:id/title) in a frame of the theme's
     * windowTitleSize and windowTitleBackgroundStyle. With FEATURE_CUSTOM_TITLE (customTitle) the frame is empty, as
     * screen_custom_title's (android:id/title_container), for the app's layout (Window.setFeatureInt). App Inventor's apps
     * hide the title by the title's parent, and without one they stopped at their start (NullPointerException in
     * Form.TitleVisible).
     */
    public FrameLayout makeContent(boolean actionBar, boolean titleBar, boolean customTitle, CharSequence title) {
        Context c = getContext();
        mStack = new LinearLayout(c);
        mStack.setOrientation(LinearLayout.VERTICAL);
        if (!actionBar && (titleBar || customTitle)) {
            TypedValue v = new TypedValue();
            int h = c.getTheme().resolveAttribute(android.R.attr.windowTitleSize, v, true) && v.type == TypedValue.TYPE_DIMENSION
                ? TypedValue.complexToDimensionPixelSize(v.data, getResources().getDisplayMetrics()) : (int) (25 * Native.density());
            int frameStyle = c.getTheme().resolveAttribute(android.R.attr.windowTitleBackgroundStyle, v, true) ? v.resourceId : 0;
            mTitleFrame = new FrameLayout(c, null, 0, frameStyle);
            if (customTitle) {
                mTitleFrame.setId(megler.IR.id.title_container);
            } else {
                int textStyle = c.getTheme().resolveAttribute(android.R.attr.windowTitleStyle, v, true) ? v.resourceId : 0;
                mPlainTitle = new TextView(c, null, 0, textStyle);
                mPlainTitle.setId(android.R.id.title);
                mPlainTitle.setBackground(null);
                mPlainTitle.setGravity(Gravity.CENTER_VERTICAL);
                mPlainTitle.setSingleLine(true);
                mPlainTitle.setEllipsize(android.text.TextUtils.TruncateAt.END);
                mPlainTitle.setText(title != null ? title : "");
                mTitleFrame.addView(mPlainTitle, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
            }
            mStack.addView(mTitleFrame, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, h));
        }
        if (actionBar) {
            TypedValue v = new TypedValue();
            int h = c.getTheme().resolveAttribute(android.R.attr.actionBarSize, v, true) ? TypedValue.complexToDimensionPixelSize(v.data, getResources().getDisplayMetrics()) : (int) (56 * Native.density());
            int bg = c.getTheme().resolveAttribute(android.R.attr.colorPrimary, v, true) && v.type >= TypedValue.TYPE_FIRST_INT ? v.data : 0xFF212121;
            /* a Toolbar with the platform's id, as the Material action bar is (apps find it by android:id/action_bar) */
            mTitle = new android.widget.Toolbar(c);
            mTitle.setId(0x010201df);
            mTitle.setTitleTextColor(Color.luminance(bg) > 0.5f ? 0xDE000000 : 0xFFFFFFFF);
            mTitle.setTitle(title != null ? title : "");
            mTitle.setBackgroundColor(bg);
            mActionBar = mTitle;
            mStack.addView(mTitle, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, h));
        }
        mContent = new FrameLayout(c) {
            @Override public void onViewAdded(View child) { super.onViewAdded(child); traceContent("added", child); }
            @Override public void onViewRemoved(View child) { super.onViewRemoved(child); traceContent("taken away", child); }
        };
        mContent.setId(android.R.id.content);
        /* Under a title bar the frame takes what is left by its weight, as screen_title's content (height 0, weight 1).
           Without one it is screen_simple's: match_parent and no weight, which as the stack's last child is the same room
           (what the bar and the tabs above it left), but told as the window was told its own -- "exactly" in a window
           of a set size, "at most" in one that wraps its content, and then not measured a second time (see the top). */
        boolean weighted = !actionBar && (titleBar || customTitle);
        mStack.addView(mContent, weighted ? new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f)
                                          : new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        addView(mStack, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        return mContent;
    }
    /*
     * trace-views: each view put into the window's content frame or taken from it, with its tags (a fragment's view carries its
     * fragment) and the stack that did it. TikTok Lite's feed played its sound under an empty black frame that something of its
     * own laid over the whole content, and the view dumps could not say what (package 631).
     */
    private static void traceContent(String what, View child) {
        if (!ViewRoot.traceViews()) return;
        StringBuilder b = new StringBuilder("the window's content: ").append(what).append(' ').append(child.getClass().getName());
        if (child.getId() != View.NO_ID) b.append(" #").append(Integer.toHexString(child.getId()));
        Drawable bg = child.getBackground();
        if (bg instanceof android.graphics.drawable.ColorDrawable) b.append(" bg=#").append(Integer.toHexString(((android.graphics.drawable.ColorDrawable) bg).getColor()));
        else if (bg != null) b.append(" bg=").append(bg.getClass().getName());
        b.append(child.meglerTagsText());
        android.util.Log.d("ViewDump", b.toString(), new Throwable("by"));
    }
    public void setTitle(CharSequence t) { if (mTitle != null) mTitle.setTitle(t); if (mPlainTitle != null) mPlainTitle.setText(t); }
    public void setTitleColor(int color) { if (mPlainTitle != null) mPlainTitle.setTextColor(color); }
    /** The custom title's frame (FEATURE_CUSTOM_TITLE), for the app's layout; null without one. */
    public FrameLayout customTitleFrame() { return mTitleFrame != null && mPlainTitle == null ? mTitleFrame : null; }
    public View actionBarView() { return mActionBar; }
    private View mTabStrip;
    /** The action bar's navigation tabs, in a row under it (null takes the row away). */
    public void setTabStrip(View strip) {
        if (mStack == null || strip == mTabStrip) return;
        if (mTabStrip != null) mStack.removeView(mTabStrip);
        mTabStrip = strip;
        if (strip != null) mStack.addView(strip, mActionBar != null ? mStack.indexOfChild(mActionBar) + 1 : 0, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, (int) (48 * Native.density())));
    }
    public void showActionBar(boolean show) { if (mActionBar != null) mActionBar.setVisibility(show ? VISIBLE : GONE); }

    /** A floating window (a dialog) is at least the theme's minimum width: a fraction of the screen's, or a size. */
    @Override protected void onMeasure(int ws, int hs) {
        if (mWindow.isFloating() && MeasureSpec.getMode(ws) == MeasureSpec.AT_MOST) {
            int sw = Native.screenWidth(), sh = Native.screenHeight();
            TypedValue tv = sw < sh ? mMinWidthMinor : mMinWidthMajor;
            int min = 0;
            if (tv != null) {
                if (tv.type == TypedValue.TYPE_DIMENSION) min = (int) tv.getDimension(getResources().getDisplayMetrics());
                else if (tv.type == TypedValue.TYPE_FRACTION) min = (int) tv.getFraction(sw, sw);
            }
            super.onMeasure(ws, hs);
            if (min > 0 && getMeasuredWidth() < min) super.onMeasure(MeasureSpec.makeMeasureSpec(Math.min(min, MeasureSpec.getSize(ws)), MeasureSpec.EXACTLY), hs);
            return;
        }
        super.onMeasure(ws, hs);
    }

    // ---- insets: the decor keeps its content off the bars unless the app lays out under them
    @Override public WindowInsets dispatchApplyWindowInsets(WindowInsets in) {
        // Each bar by itself (Window.meglerFitsStatusBar, overlaid members): the navigation bar's side goes with it in landscape.
        ViewRoot root = ViewRoot.of(this);
        int sys = root != null ? root.systemUi() : 0;
        boolean floating = mWindow.isFloating();
        boolean top = !floating && mWindow.meglerFitsStatusBar(sys), bottom = !floating && mWindow.meglerFitsNavigationBar(sys);
        sayFits(top, bottom, sys);
        if (top && bottom) {
            mInsetLeft = in.getSystemWindowInsetLeft(); mInsetTop = mWindow.meglerFullscreen() ? 0 : in.getSystemWindowInsetTop();
            mInsetRight = in.getSystemWindowInsetRight(); mInsetBottom = in.getSystemWindowInsetBottom();
            FrameLayout.LayoutParams lp = stackParams();
            if (lp != null) {
                lp.setMargins(mInsetLeft, mInsetTop, mInsetRight, mInsetBottom);
                mStack.setLayoutParams(lp);
            }
            // (They still reach every view of the content, with nothing left in them: WindowInsets.meglerToChildren.)
            WindowInsets rest = in.consumeSystemWindowInsets().meglerToChildren();
            super.dispatchApplyWindowInsets(rest);
            return rest;
        }
        if (top || bottom) {
            // One bar kept off, the other left to the app: the content's margin for the one, the insets of the other passed on.
            int l = in.getSystemWindowInsetLeft(), t = in.getSystemWindowInsetTop(), r = in.getSystemWindowInsetRight(), b = in.getSystemWindowInsetBottom();
            mInsetTop = top && !mWindow.meglerFullscreen() ? t : 0;
            mInsetLeft = bottom ? l : 0; mInsetRight = bottom ? r : 0; mInsetBottom = bottom ? b : 0;
            FrameLayout.LayoutParams lp = stackParams();
            if (lp != null) {
                lp.setMargins(mInsetLeft, mInsetTop, mInsetRight, mInsetBottom);
                mStack.setLayoutParams(lp);
            }
            WindowInsets rest = in.replaceSystemWindowInsets(l - mInsetLeft, top ? 0 : t, r - mInsetRight, b - mInsetBottom);
            super.dispatchApplyWindowInsets(rest);
            return rest;
        }
        FrameLayout.LayoutParams lp = stackParams();
        if (lp != null) { lp.setMargins(0, 0, 0, 0); mStack.setLayoutParams(lp); }
        mInsetTop = mInsetBottom = mInsetLeft = mInsetRight = 0;
        return super.dispatchApplyWindowInsets(in);
    }
    /* What the decor keeps the content off, said in the log when it changes, with what decided it. */
    private int mSaidFits = -1;
    private void sayFits(boolean top, boolean bottom, int sys) {
        int fits = (top ? 1 : 0) | (bottom ? 2 : 0);
        if (fits == mSaidFits || mWindow.isFloating()) return;
        mSaidFits = fits;
        android.util.Log.d("Megler", "decor of " + getContext().getClass().getName() + ": keeps the content off the status bar " + top
                + ", off the navigation bar " + bottom + " (" + mWindow.meglerFitsWhy(sys) + ")");
    }
    /* The content stack's layout parameters while it is still the decor's own child; null once an app has moved it elsewhere. */
    private FrameLayout.LayoutParams stackParams() {
        if (mStack == null || mStack.getParent() != this || !(mStack.getLayoutParams() instanceof FrameLayout.LayoutParams)) return null;
        return (FrameLayout.LayoutParams) mStack.getLayoutParams();
    }
    /* The theme's windowBackgroundFallback, read once, the first time the window has no background to draw. */
    private Drawable mFallback;
    private boolean mFallbackRead;
    private Drawable backgroundFallback() {
        if (!mFallbackRead) {
            mFallbackRead = true;
            try {
                TypedArray a = getContext().obtainStyledAttributes(new int[] { android.R.attr.windowBackgroundFallback });
                mFallback = a.getDrawable(0);
                a.recycle();
            } catch (RuntimeException e) {
                mFallback = null;
            }
        }
        return mFallback;
    }
    @Override public void draw(Canvas c) {
        // A window whose surface the app has taken (Window.takeSurface, NativeActivity's): Android draws none of its views --
        // "the app owns the surface, we won't draw" (ViewRootImpl.draw) --, its background, title bar and bars' colours
        // included; the surface, the SurfaceView that stands for it here, is all there is to see.
        View taken = mWindow.meglerTakenSurfaceView();
        if (taken != null && taken.getParent() == this) {
            drawChild(c, taken, getDrawingTime());
            return;
        }
        if (mBackground != null) {
            mBackground.setBounds(0, 0, getWidth(), getHeight());
            mBackground.draw(c);
        } else if (!mWindow.isFloating() && stackParams() != null) {
            // No window background: the theme's fallback where the content stack does not reach (Android's BackgroundFallback,
            // which fills the decor's padding around its content root the same way).
            Drawable f = backgroundFallback();
            int l = mStack.getLeft(), t = mStack.getTop(), r = mStack.getRight(), b = mStack.getBottom(), w = getWidth(), h = getHeight();
            if (f != null && r > l && b > t) {
                if (t > 0) { f.setBounds(0, 0, w, t); f.draw(c); }
                if (b < h) { f.setBounds(0, b, w, h); f.draw(c); }
                if (l > 0) { f.setBounds(0, t, l, b); f.draw(c); }
                if (r < w) { f.setBounds(r, t, w, b); f.draw(c); }
            }
        }
        super.draw(c);
        // the bars' own colours over the areas the content was kept out of
        if (mInsetTop > 0 && !mWindow.isFloating()) { mBarPaint.setColor(mWindow.getStatusBarColor()); c.drawRect(0, 0, getWidth(), mInsetTop, mBarPaint); }
        if (mInsetBottom > 0 && !mWindow.isFloating()) { mBarPaint.setColor(mWindow.getNavigationBarColor()); c.drawRect(0, getHeight() - mInsetBottom, getWidth(), getHeight(), mBarPaint); }
    }

    // ---- events
    @Override public boolean dispatchTouchEvent(MotionEvent e) {
        Window.Callback cb = mWindow.getCallback();
        if (e.getActionMasked() != MotionEvent.ACTION_OUTSIDE && mWindow.meglerTakenInput(e)) return true;
        if (e.getActionMasked() == MotionEvent.ACTION_OUTSIDE || (mCloseOnTouchOutside && e.getActionMasked() == MotionEvent.ACTION_DOWN && outside(e))) {
            if (cb != null && !mWindow.isDestroyed()) cb.dispatchTouchEvent(e);
            return true;
        }
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchTouchEvent(e) : super.dispatchTouchEvent(e);
    }
    private boolean outside(MotionEvent e) { return e.getX() < 0 || e.getY() < 0 || e.getX() >= getWidth() || e.getY() >= getHeight(); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (mWindow.meglerTakenInput(e)) return true;
        Window.Callback cb = mWindow.getCallback();
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchKeyEvent(e) : super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        Window.Callback cb = mWindow.getCallback();
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchGenericMotionEvent(e) : super.dispatchGenericMotionEvent(e);
    }
    public boolean superDispatchTouchEvent(MotionEvent e) { return super.dispatchTouchEvent(e); }
    public boolean superDispatchKeyEvent(KeyEvent e) {
        if (super.dispatchKeyEvent(e)) return true;
        return false;
    }
    public boolean superDispatchGenericMotionEvent(MotionEvent e) { return super.dispatchGenericMotionEvent(e); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); Window.Callback cb = mWindow.getCallback(); if (cb != null && !mWindow.isDestroyed()) cb.onWindowFocusChanged(f); }
    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        mWindow.meglerAttached();
        Window.Callback cb = mWindow.getCallback();
        if (cb != null && !mWindow.isDestroyed()) cb.onAttachedToWindow();
    }
    @Override protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        Window.Callback cb = mWindow.getCallback();
        if (cb != null) cb.onDetachedFromWindow();
        mWindow.meglerDetached();
    }
    public Window getWindowMegler() { return mWindow; }
    /** The window's background it draws (the theme's windowBackground, or what the app set), for trace-views. */
    public Drawable meglerWindowBackground() { return mBackground; }
}
