// Overlay of core/framework/gen-src/android/app/TabActivity.java (tools/build-framework.sh).
// It differs in being Android's: an ActivityGroup whose content is a TabHost (android:id/tabhost: the app's layout's, else
// the platform's tab_content), set up with the group's LocalActivityManager so that a tab may be an activity. The generated
// stub had no TabHost (getTabHost gave null), and the apps of tabs from before fragments stopped at their start.
package android.app;

import android.os.Bundle;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TabHost;
import android.widget.TabWidget;

public class TabActivity extends ActivityGroup {
    private TabHost mTabHost;
    private String mDefaultTab = null;
    private int mDefaultTabIndex = -1;

    public TabActivity() { }

    public void setDefaultTab(String tag) {
        mDefaultTab = tag;
        mDefaultTabIndex = -1;
    }

    public void setDefaultTab(int index) {
        mDefaultTab = null;
        mDefaultTabIndex = index;
    }

    @Override
    protected void onRestoreInstanceState(Bundle state) {
        super.onRestoreInstanceState(state);
        ensureTabHost();
        String cur = state.getString("currentTab");
        if (cur != null) mTabHost.setCurrentTabByTag(cur);
        if (mTabHost.getCurrentTab() < 0) {
            if (mDefaultTab != null) mTabHost.setCurrentTabByTag(mDefaultTab);
            else if (mDefaultTabIndex >= 0) mTabHost.setCurrentTab(mDefaultTabIndex);
        }
    }

    @Override
    protected void onPostCreate(Bundle icicle) {
        super.onPostCreate(icicle);
        ensureTabHost();
        if (mTabHost.getCurrentTab() == -1) mTabHost.setCurrentTab(0);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        String currentTabTag = mTabHost != null ? mTabHost.getCurrentTabTag() : null;
        if (currentTabTag != null) outState.putString("currentTab", currentTabTag);
    }

    @Override
    public void onContentChanged() {
        super.onContentChanged();
        mTabHost = findViewById(android.R.id.tabhost);
        if (mTabHost == null) throw new RuntimeException("Your content must have a TabHost whose id attribute is 'android.R.id.tabhost'");
        mTabHost.setup(getLocalActivityManager());
    }

    private void ensureTabHost() {
        if (mTabHost != null) return;
        View content;
        try {
            content = getLayoutInflater().inflate(megler.IR.layout.tab_content, null);
        } catch (RuntimeException e) {
            content = null;
        }
        if (content == null || content.findViewById(android.R.id.tabhost) == null) {
            // (The platform's tab_content, made here: a TabWidget over the content frame.)
            TabHost host = new TabHost(this);
            host.setId(android.R.id.tabhost);
            LinearLayout column = new LinearLayout(this);
            column.setOrientation(LinearLayout.VERTICAL);
            TabWidget tabs = new TabWidget(this);
            tabs.setId(android.R.id.tabs);
            column.addView(tabs, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT, 0));
            FrameLayout frame = new FrameLayout(this);
            frame.setId(android.R.id.tabcontent);
            column.addView(frame, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
            host.addView(column, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            content = host;
        }
        setContentView(content);
    }

    @Override
    protected void onChildTitleChanged(Activity childActivity, CharSequence title) {
        // (The tab's own title, when its indicator has one, as Android's.)
        if (getLocalActivityManager().getCurrentActivity() == childActivity) {
            View tabView = mTabHost.getCurrentTabView();
            if (tabView instanceof android.widget.TextView) ((android.widget.TextView) tabView).setText(title);
        }
    }

    public TabHost getTabHost() {
        ensureTabHost();
        return mTabHost;
    }

    public TabWidget getTabWidget() { return mTabHost.getTabWidget(); }
}
