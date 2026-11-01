// Overlay of core/framework/gen-src/android/app/ListFragment.java (tools/build-framework.sh), which had the signatures alone:
// getListView answered null, and Gmail's folder list (com.android.mail.ui.FolderListFragment) ended at its first account with
// a NullPointerException in ListView.setSelection (package 511). This is Android's: the list is the fragment's view's
// android.R.id.list, or the view itself when it is a ListView, found once the view is made (onViewCreated, getListView); a
// fragment that makes no view of its own gets one of the list's, with a progress view and an empty text (onCreateView). The
// adapter, the clicks, the empty view and the list shown or not are as Android has them.
package android.app;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListAdapter;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.TextView;

public class ListFragment extends Fragment {
    private final Handler mHandler = new Handler();
    private final Runnable mRequestFocus = new Runnable() {
        @Override public void run() { if (mList != null) mList.focusableViewAvailable(mList); }
    };
    private final AdapterView.OnItemClickListener mOnClickListener = new AdapterView.OnItemClickListener() {
        @Override public void onItemClick(AdapterView<?> parent, View v, int position, long id) { onListItemClick((ListView) parent, v, position, id); }
    };

    ListAdapter mAdapter;
    ListView mList;
    View mEmptyView;
    TextView mStandardEmptyView;
    View mProgressContainer;
    View mListContainer;
    CharSequence mEmptyText;
    boolean mListShown;
    /* The view of its own made by onCreateView, and its parts: what Android finds by its internal ids. */
    private View mOwnRoot, mOwnProgress, mOwnListContainer;
    private TextView mOwnEmpty;

    public ListFragment() {}

    /** A view with the list in it, a progress view and an empty text, as Android's list_content. */
    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        final Context context = inflater.getContext();
        final int match = ViewGroup.LayoutParams.MATCH_PARENT, wrap = ViewGroup.LayoutParams.WRAP_CONTENT;
        FrameLayout root = new FrameLayout(context);
        LinearLayout progress = new LinearLayout(context);
        progress.setOrientation(LinearLayout.VERTICAL);
        progress.setVisibility(View.GONE);
        progress.setGravity(Gravity.CENTER);
        progress.addView(new ProgressBar(context, null, android.R.attr.progressBarStyleLarge), new FrameLayout.LayoutParams(wrap, wrap));
        root.addView(progress, new FrameLayout.LayoutParams(match, match));
        FrameLayout lframe = new FrameLayout(context);
        TextView empty = new TextView(context);
        empty.setGravity(Gravity.CENTER);
        lframe.addView(empty, new FrameLayout.LayoutParams(match, match));
        ListView list = new ListView(context);
        list.setId(android.R.id.list);
        list.setDrawSelectorOnTop(false);
        lframe.addView(list, new FrameLayout.LayoutParams(match, match));
        root.addView(lframe, new FrameLayout.LayoutParams(match, match));
        root.setLayoutParams(new FrameLayout.LayoutParams(match, match));
        mOwnRoot = root;
        mOwnProgress = progress;
        mOwnListContainer = lframe;
        mOwnEmpty = empty;
        return root;
    }

    @Override
    public void onViewCreated(View view, Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        ensureList();
    }

    @Override
    public void onDestroyView() {
        mHandler.removeCallbacks(mRequestFocus);
        mList = null;
        mListShown = false;
        mEmptyView = mProgressContainer = mListContainer = null;
        mStandardEmptyView = null;
        mOwnRoot = mOwnProgress = mOwnListContainer = null;
        mOwnEmpty = null;
        super.onDestroyView();
    }

    public void onListItemClick(ListView l, View v, int position, long id) {}

    public void setListAdapter(ListAdapter adapter) {
        boolean hadAdapter = mAdapter != null;
        mAdapter = adapter;
        if (mList != null) {
            mList.setAdapter(adapter);
            if (!mListShown && !hadAdapter) {
                // The list was hidden, and an adapter is set for the first time: shown now.
                View v = getView();
                setListShown(true, v != null && v.getWindowToken() != null);
            }
        }
    }

    public void setSelection(int position) {
        ensureList();
        mList.setSelection(position);
    }

    public int getSelectedItemPosition() {
        ensureList();
        return mList.getSelectedItemPosition();
    }

    public long getSelectedItemId() {
        ensureList();
        return mList.getSelectedItemId();
    }

    public ListView getListView() {
        ensureList();
        return mList;
    }

    public void setEmptyText(CharSequence text) {
        ensureList();
        if (mStandardEmptyView == null) throw new IllegalStateException("Can't be used with a custom content view");
        mStandardEmptyView.setText(text);
        if (mEmptyText == null) mList.setEmptyView(mStandardEmptyView);
        mEmptyText = text;
    }

    public void setListShown(boolean shown) { setListShown(shown, true); }

    public void setListShownNoAnimation(boolean shown) { setListShown(shown, false); }

    private void setListShown(boolean shown, boolean animate) {
        ensureList();
        if (mProgressContainer == null) throw new IllegalStateException("Can't be used with a custom content view");
        if (mListShown == shown) return;
        mListShown = shown;
        // (Android fades between the two; here they change at once.)
        mProgressContainer.setVisibility(shown ? View.GONE : View.VISIBLE);
        mListContainer.setVisibility(shown ? View.VISIBLE : View.GONE);
    }

    public ListAdapter getListAdapter() { return mAdapter; }

    private void ensureList() {
        if (mList != null) return;
        View root = getView();
        if (root == null) throw new IllegalStateException("Content view not yet created");
        if (root instanceof ListView) {
            mList = (ListView) root;
        } else {
            mStandardEmptyView = root == mOwnRoot ? mOwnEmpty : null;
            if (mStandardEmptyView == null) mEmptyView = root.findViewById(android.R.id.empty);
            else mStandardEmptyView.setVisibility(View.GONE);
            mProgressContainer = root == mOwnRoot ? mOwnProgress : null;
            mListContainer = root == mOwnRoot ? mOwnListContainer : null;
            View raw = root.findViewById(android.R.id.list);
            if (!(raw instanceof ListView)) {
                if (raw == null) throw new RuntimeException("Your content must have a ListView whose id attribute is 'android.R.id.list'");
                throw new RuntimeException("Content has view with id attribute 'android.R.id.list' that is not a ListView class");
            }
            mList = (ListView) raw;
            if (mEmptyView != null) {
                mList.setEmptyView(mEmptyView);
            } else if (mEmptyText != null) {
                mStandardEmptyView.setText(mEmptyText);
                mList.setEmptyView(mStandardEmptyView);
            }
        }
        mListShown = true;
        mList.setOnItemClickListener(mOnClickListener);
        if (mAdapter != null) {
            ListAdapter adapter = mAdapter;
            mAdapter = null;
            setListAdapter(adapter);
        } else if (mProgressContainer != null) {
            // No adapter yet: the progress view, until there is one.
            setListShown(false, false);
        }
        mHandler.post(mRequestFocus);
    }
}
