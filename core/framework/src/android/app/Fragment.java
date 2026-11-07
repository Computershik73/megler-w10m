package android.app;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.view.*;

/** The platform's (deprecated) fragments: a lifecycle tied to their activity's, a view put in a container. */
public class Fragment implements android.content.ComponentCallbacks2, View.OnCreateContextMenuListener {
    /** What FragmentManager.saveFragmentInstanceState kept of a fragment, for setInitialSavedState: its onSaveInstanceState. */
    public static class SavedState implements android.os.Parcelable {
        final Bundle mState;
        SavedState(Bundle state) { mState = state; }
        SavedState(android.os.Parcel in, ClassLoader loader) { mState = in.readBundle(loader); }
        public int describeContents() { return 0; }
        public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeBundle(mState); }
        public static final android.os.Parcelable.ClassLoaderCreator<SavedState> CREATOR = new android.os.Parcelable.ClassLoaderCreator<SavedState>() {
            public SavedState createFromParcel(android.os.Parcel in) { return new SavedState(in, null); }
            public SavedState createFromParcel(android.os.Parcel in, ClassLoader loader) { return new SavedState(in, loader); }
            public SavedState[] newArray(int size) { return new SavedState[size]; }
        };
    }
    public static class InstantiationException extends android.util.AndroidRuntimeException { public InstantiationException(String m, Exception e) { super(m, e); } }
    Activity mActivity;
    FragmentManager mFragmentManager;
    Bundle mArguments;
    /* the state the fragment is created from (setInitialSavedState): onCreate's and the view's savedInstanceState, null for a new one */
    Bundle mSavedFragmentState;
    View mView;
    ViewGroup mContainer;
    int mFragmentId, mContainerId;
    String mTag;
    boolean mMeglerActivityCreated;
    /* the saved state it was created from, until onActivityCreated and onViewStateRestored have it too (FragmentManager.raise) */
    Bundle mMeglerRestored;
    boolean mAdded, mRemoving, mDetached, mHidden, mResumed, mStarted, mInLayout, mRetainInstance, mHasMenu = false, mMenuVisible = true, mUserVisibleHint = true, mCalled;
    Fragment mTarget;
    int mTargetRequestCode;
    public Fragment() {}
    public static Fragment instantiate(Context c, String name) { return instantiate(c, name, null); }
    public static Fragment instantiate(Context c, String name, Bundle args) {
        try { Fragment f = (Fragment) Class.forName(name).newInstance(); if (args != null) f.setArguments(args); return f; }
        catch (Exception e) { throw new InstantiationException("Unable to instantiate fragment " + name, e); }
    }
    public final boolean equals(Object o) { return super.equals(o); }
    public final int hashCode() { return super.hashCode(); }
    public final int getId() { return mFragmentId; }
    public final String getTag() { return mTag; }
    public void setArguments(Bundle a) { mArguments = a; }
    public final Bundle getArguments() { return mArguments; }
    public final boolean isStateSaved() { return false; }
    public void setInitialSavedState(SavedState s) {
        if (mAdded) throw new IllegalStateException("Fragment already active");
        mSavedFragmentState = s != null ? s.mState : null;
    }
    public void setTargetFragment(Fragment f, int request) { mTarget = f; mTargetRequestCode = request; }
    public final Fragment getTargetFragment() { return mTarget; }
    public final int getTargetRequestCode() { return mTargetRequestCode; }
    public Context getContext() { return mActivity; }
    public final Activity getActivity() { return mActivity; }
    public final Object getHost() { return mActivity; }
    public final android.content.res.Resources getResources() { return mActivity.getResources(); }
    public final CharSequence getText(int id) { return getResources().getText(id); }
    public final String getString(int id) { return getResources().getString(id); }
    public final String getString(int id, Object... args) { return getResources().getString(id, args); }
    public final FragmentManager getFragmentManager() { return mFragmentManager; }
    public final FragmentManager getChildFragmentManager() { return mActivity.getFragmentManager(); }
    public final Fragment getParentFragment() { return null; }
    public final boolean isAdded() { return mActivity != null && mAdded; }
    public final boolean isDetached() { return mDetached; }
    public final boolean isRemoving() { return mRemoving; }
    public final boolean isInLayout() { return mInLayout; }
    public final boolean isResumed() { return mResumed; }
    public final boolean isVisible() { return isAdded() && !mHidden && mView != null && mView.getVisibility() == View.VISIBLE; }
    public final boolean isHidden() { return mHidden; }
    public void onHiddenChanged(boolean hidden) {}
    public void setRetainInstance(boolean r) { mRetainInstance = r; }
    public final boolean getRetainInstance() { return mRetainInstance; }
    public void setHasOptionsMenu(boolean h) { mHasMenu = h; }
    public void setMenuVisibility(boolean v) { mMenuVisible = v; }
    public void setUserVisibleHint(boolean v) { mUserVisibleHint = v; }
    public boolean getUserVisibleHint() { return mUserVisibleHint; }
    private LoaderManagerImpl mLoaders;
    public LoaderManager getLoaderManager() { if (mLoaders == null) mLoaders = new LoaderManagerImpl(true); return mLoaders; }
    public void startActivity(Intent i) { mActivity.startActivity(i); }
    public void startActivity(Intent i, Bundle o) { mActivity.startActivity(i, o); }
    public void startActivityForResult(Intent i, int r) { mActivity.startActivityForResult(i, r); }
    public void startActivityForResult(Intent i, int r, Bundle o) { mActivity.startActivityForResult(i, r, o); }
    public void onActivityResult(int r, int res, Intent d) {}
    /* A fragment's request is answered to the fragment, as Android's FragmentHostCallback has it (megler.Permissions); one not
       attached to an activity is refused, as Android refuses it. */
    public final void requestPermissions(String[] p, int r) {
        if (mActivity == null) throw new IllegalStateException("Fragment " + this + " not attached to Activity");
        mActivity.meglerRequestPermissions(p, r, this);
    }
    public void onRequestPermissionsResult(int r, String[] p, int[] g) {}
    public boolean shouldShowRequestPermissionRationale(String p) { return mActivity != null && mActivity.shouldShowRequestPermissionRationale(p); }
    public LayoutInflater onGetLayoutInflater(Bundle s) { return mActivity.getLayoutInflater(); }
    public void onInflate(Context c, android.util.AttributeSet a, Bundle s) { mCalled = true; }
    public void onInflate(Activity a, android.util.AttributeSet at, Bundle s) { mCalled = true; }
    public void onAttachFragment(Fragment f) {}
    public void onAttach(Context c) { mCalled = true; }
    public void onAttach(Activity a) { mCalled = true; }
    public android.animation.Animator onCreateAnimator(int transit, boolean enter, int nextAnim) { return null; }
    public void onCreate(Bundle s) { mCalled = true; }
    public View onCreateView(LayoutInflater i, ViewGroup c, Bundle s) { return null; }
    public void onViewCreated(View v, Bundle s) {}
    public View getView() { return mView; }
    public void onActivityCreated(Bundle s) { mCalled = true; }
    public void onViewStateRestored(Bundle s) { mCalled = true; }
    public void onStart() { mCalled = true; }
    public void onResume() { mCalled = true; }
    public void onSaveInstanceState(Bundle out) {}
    public void onMultiWindowModeChanged(boolean m) {}
    public void onPictureInPictureModeChanged(boolean m) {}
    public void onConfigurationChanged(android.content.res.Configuration c) { mCalled = true; }
    public void onPause() { mCalled = true; }
    public void onStop() { mCalled = true; }
    public void onLowMemory() { mCalled = true; }
    public void onTrimMemory(int l) { mCalled = true; }
    public void onDestroyView() { mCalled = true; }
    public void onDestroy() { mCalled = true; }
    public void onDetach() { mCalled = true; }
    public void onCreateOptionsMenu(Menu m, MenuInflater i) {}
    public void onPrepareOptionsMenu(Menu m) {}
    public void onDestroyOptionsMenu() {}
    public boolean onOptionsItemSelected(MenuItem i) { return false; }
    public void onOptionsMenuClosed(Menu m) {}
    public void onCreateContextMenu(ContextMenu m, View v, ContextMenu.ContextMenuInfo i) {}
    public void registerForContextMenu(View v) { v.setOnCreateContextMenuListener(this); }
    public void unregisterForContextMenu(View v) { v.setOnCreateContextMenuListener(null); }
    public boolean onContextItemSelected(MenuItem i) { return false; }
    public void setEnterSharedElementCallback(SharedElementCallback c) {}
    public void setExitSharedElementCallback(SharedElementCallback c) {}
    public void setEnterTransition(android.transition.Transition t) {} public void setReturnTransition(android.transition.Transition t) {} public void setExitTransition(android.transition.Transition t) {} public void setReenterTransition(android.transition.Transition t) {}
    public void setSharedElementEnterTransition(android.transition.Transition t) {} public void setSharedElementReturnTransition(android.transition.Transition t) {} public void postponeEnterTransition() {} public void startPostponedEnterTransition() {}
    public void setAllowEnterTransitionOverlap(boolean b) {} public void setAllowReturnTransitionOverlap(boolean b) {}
    public void dump(String p, java.io.FileDescriptor fd, java.io.PrintWriter w, String[] a) {}
    @Override public String toString() { return getClass().getSimpleName() + "{" + Integer.toHexString(System.identityHashCode(this)) + (mFragmentId != 0 ? " #" + Integer.toHexString(mFragmentId) : "") + (mTag != null ? " " + mTag : "") + "}"; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public boolean getAllowEnterTransitionOverlap() { return false; }
    public boolean getAllowReturnTransitionOverlap() { return false; }
    public android.transition.Transition getEnterTransition() { return null; }
    public android.transition.Transition getExitTransition() { return null; }
    public android.view.LayoutInflater getLayoutInflater() { return null; }
    public android.transition.Transition getReenterTransition() { return null; }
    public android.transition.Transition getReturnTransition() { return null; }
    public android.transition.Transition getSharedElementEnterTransition() { return null; }
    public android.transition.Transition getSharedElementReturnTransition() { return null; }
    public void onInflate(android.util.AttributeSet p0, android.os.Bundle p1) {}
    public void onMultiWindowModeChanged(boolean p0, android.content.res.Configuration p1) {}
    public void onPictureInPictureModeChanged(boolean p0, android.content.res.Configuration p1) {}
    public void startActivityForResultAsUser(android.content.Intent p0, int p1, android.os.Bundle p2, android.os.UserHandle p3) {}
    public void startIntentSenderForResult(android.content.IntentSender p0, int p1, android.content.Intent p2, int p3, int p4, int p5, android.os.Bundle p6) {}
    // ---- end of generated members
}
