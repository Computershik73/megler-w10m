package android.widget;

import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** Tabs over a content frame (the layout's android:id/tabs TabWidget and android:id/tabcontent FrameLayout): each tab shows a view of
 *  the layout, one its factory makes, or an activity's (an intent: run by the LocalActivityManager setup was given, whose
 *  window's decor is shown, as Android's IntentContentStrategy). */
public class TabHost extends FrameLayout implements android.view.ViewTreeObserver.OnTouchModeChangeListener {
    public interface OnTabChangeListener { void onTabChanged(String tabId); }
    public interface TabContentFactory { View createTabContent(String tag); }

    private TabWidget mTabWidget;
    private FrameLayout mContent;
    private final ArrayList<TabSpec> mSpecs = new ArrayList<>();
    private int mCurrent = -1;
    private View mCurrentView;
    private OnTabChangeListener mListener;

    public TabHost(Context c) { super(c); }
    public TabHost(Context c, AttributeSet a) { super(c, a); }
    public TabHost(Context c, AttributeSet a, int s) { super(c, a, s); }
    public TabHost(Context c, AttributeSet a, int s, int r) { super(c, a, s, r); }

    public void setup() {
        mTabWidget = findViewById(android.R.id.tabs);
        if (mTabWidget == null) throw new RuntimeException("Your TabHost must have a TabWidget whose id attribute is 'android.R.id.tabs'");
        mContent = findViewById(android.R.id.tabcontent);
        if (mContent == null) throw new RuntimeException("Your TabHost must have a FrameLayout whose id attribute is 'android.R.id.tabcontent'");
        mTabWidget.meglerSetHost(this);
    }
    public void setup(android.app.LocalActivityManager m) { setup(); mLocalActivityManager = m; }
    public TabSpec newTabSpec(String tag) { if (tag == null) throw new IllegalArgumentException("tag must be non-null"); return new TabSpec(tag); }
    public void addTab(TabSpec spec) {
        if (spec.indicator == null) throw new IllegalArgumentException("you must specify a way to create the tab indicator.");
        if (spec.content == null && spec.contentId == 0 && spec.factory == null) throw new IllegalArgumentException("you must specify a way to create the tab content");
        mTabWidget.meglerAddTab(spec.indicator(getContext()));
        mSpecs.add(spec);
        if (spec.contentId != 0) { View v = mContent.findViewById(spec.contentId); if (v != null) v.setVisibility(GONE); }
        if (mCurrent == -1) setCurrentTab(0);
    }
    public void clearAllTabs() { mTabWidget.removeAllViews(); mSpecs.clear(); mContent.removeAllViews(); mCurrent = -1; mCurrentView = null; requestLayout(); }
    public TabWidget getTabWidget() { return mTabWidget; }
    public int getCurrentTab() { return mCurrent; }
    public String getCurrentTabTag() { return mCurrent >= 0 && mCurrent < mSpecs.size() ? mSpecs.get(mCurrent).tag : null; }
    public View getCurrentTabView() { return mCurrent >= 0 && mCurrent < mSpecs.size() ? mTabWidget.getChildTabViewAt(mCurrent) : null; }
    public View getCurrentView() { return mCurrentView; }
    public FrameLayout getTabContentView() { return mContent; }
    public void setCurrentTabByTag(String tag) { for (int i = 0; i < mSpecs.size(); i++) if (mSpecs.get(i).tag.equals(tag)) { setCurrentTab(i); return; } }
    public void setOnTabChangedListener(OnTabChangeListener l) { mListener = l; }
    public void setCurrentTab(int index) {
        if (index < 0 || index >= mSpecs.size() || index == mCurrent) return;
        if (mCurrentView != null) {
            TabSpec old = mSpecs.get(mCurrent);
            // (An activity's decor stays, hidden, as Android's tabClosed leaves it.)
            if (old.contentId != 0 || old.content != null) mCurrentView.setVisibility(GONE); else mContent.removeView(mCurrentView);
        }
        mCurrent = index;
        TabSpec s = mSpecs.get(index);
        mTabWidget.focusCurrentTab(index);
        View v;
        if (s.contentId != 0) { v = mContent.findViewById(s.contentId); if (v != null) v.setVisibility(VISIBLE); }
        else if (s.content != null) {
            if (mLocalActivityManager == null) throw new IllegalStateException("Did you forget to call 'public void setup(LocalActivityManager activityGroup)'?");
            android.view.Window w = mLocalActivityManager.startActivity(s.tag, s.content);
            View decor = w != null ? w.getDecorView() : null;
            if (s.made != decor && s.made != null && s.made.getParent() == mContent) mContent.removeView(s.made);
            s.made = decor;
            v = decor != null ? decor : new View(getContext());
            v.setVisibility(VISIBLE);
            v.setFocusableInTouchMode(true);
            if (v instanceof ViewGroup) ((ViewGroup) v).setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
            if (v.getParent() == null) mContent.addView(v, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        } else {
            if (s.made == null) s.made = s.factory != null ? s.factory.createTabContent(s.tag) : new View(getContext());
            v = s.made;
            if (v.getParent() == null) mContent.addView(v, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        mCurrentView = v;
        if (mListener != null) mListener.onTabChanged(s.tag);
    }
    public void onTouchModeChanged(boolean isInTouchMode) {}

    public class TabSpec {
        final String tag;
        CharSequence label; Drawable icon; View view;
        Object indicator;
        int contentId; TabContentFactory factory; Intent content;
        View made;
        TabSpec(String tag) { this.tag = tag; }
        public String getTag() { return tag; }
        public TabSpec setIndicator(CharSequence label) { this.label = label; indicator = label; return this; }
        public TabSpec setIndicator(CharSequence label, Drawable icon) { this.label = label; this.icon = icon; indicator = label; return this; }
        public TabSpec setIndicator(View view) { this.view = view; indicator = view; return this; }
        public TabSpec setContent(int viewId) { contentId = viewId; return this; }
        public TabSpec setContent(TabContentFactory f) { factory = f; return this; }
        public TabSpec setContent(Intent i) { content = i; return this; }
        View indicator(Context c) {
            if (view != null) return view;
            float d = c.getResources().getDisplayMetrics().density;
            TextView t = new TextView(c);
            t.setId(android.R.id.title);
            t.setText(label != null ? label.toString().toUpperCase(java.util.Locale.getDefault()) : "");
            t.setGravity(Gravity.CENTER);
            t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            t.setPadding((int) (12 * d), (int) (12 * d), (int) (12 * d), (int) (12 * d));
            if (icon != null) t.setCompoundDrawablesWithIntrinsicBounds(null, icon, null, null);
            return t;
        }
    }
    protected android.app.LocalActivityManager mLocalActivityManager;
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    protected int mCurrentTab;
    public void sendAccessibilityEventInternal(int p0) {}
    // ---- end of generated members
}
