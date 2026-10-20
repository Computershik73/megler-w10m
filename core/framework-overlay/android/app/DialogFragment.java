// Overlay: android.app.DialogFragment, in place of the generated stub (core/framework/gen-src).
//
// Why. The stub had the class's signatures and nothing behind them: show() did nothing, onCreateDialog was never called,
// getDialog() was always null. An app whose dialog is a platform DialogFragment showed nothing where the dialog should be.
// VK mp3 mod's sign-in, when VK asks for a confirmation code, shows AuthCheckFragment (a DialogFragment whose
// onCreateDialog builds an AlertDialog with the code's field) over TransientAuthActivity, whose window is otherwise empty:
// the user saw a black screen and nothing more happened.
//
// This is Android's class (Android 9's), on the original's FragmentManager: show() adds the fragment with no container and
// commits; the dialog is made by onCreateDialog, given the fragment's view if it made one, shown in onStart, hidden in
// onStop, and dismissed with the fragment's view; dismiss() takes the fragment away again, by its back stack entry when
// show(FragmentTransaction, String) put it on the back stack. Two things differ because the original's FragmentManager
// differs from Android's: it does not ask a fragment for its layout inflater before onCreateView, so the dialog is made in
// onActivityCreated where it was not made already (onGetLayoutInflater makes it too, as on Android, for whoever asks);
// and it calls onDestroyView only for a fragment with a view, so onDestroy dismisses a dialog that is still there.
package android.app;

import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

public class DialogFragment extends Fragment implements DialogInterface.OnCancelListener, DialogInterface.OnDismissListener {
    public static final int STYLE_NORMAL = 0;
    public static final int STYLE_NO_TITLE = 1;
    public static final int STYLE_NO_FRAME = 2;
    public static final int STYLE_NO_INPUT = 3;

    private static final String SAVED_DIALOG_STATE_TAG = "android:savedDialogState";
    private static final String SAVED_STYLE = "android:style";
    private static final String SAVED_THEME = "android:theme";
    private static final String SAVED_CANCELABLE = "android:cancelable";
    private static final String SAVED_SHOWS_DIALOG = "android:showsDialog";
    private static final String SAVED_BACK_STACK_ID = "android:backStackId";

    int mStyle = STYLE_NORMAL;
    int mTheme = 0;
    boolean mCancelable = true;
    boolean mShowsDialog = true;
    int mBackStackId = -1;
    Dialog mDialog;
    boolean mViewDestroyed;
    boolean mDismissed;
    boolean mShownByMe;

    public DialogFragment() {}

    public void setStyle(int style, int theme) {
        mStyle = style;
        if (theme != 0) mTheme = theme;
    }

    public void show(FragmentManager manager, String tag) {
        mDismissed = false;
        mShownByMe = true;
        FragmentTransaction ft = manager.beginTransaction();
        ft.add(this, tag);
        ft.commit();
    }

    public void showAllowingStateLoss(FragmentManager manager, String tag) {
        mDismissed = false;
        mShownByMe = true;
        FragmentTransaction ft = manager.beginTransaction();
        ft.add(this, tag);
        ft.commitAllowingStateLoss();
    }

    public int show(FragmentTransaction transaction, String tag) {
        mDismissed = false;
        mShownByMe = true;
        transaction.add(this, tag);
        mViewDestroyed = false;
        // The original's commit() answers 0 whether or not the transaction is on the back stack; Android's, -1 when not.
        boolean onBackStack = transaction instanceof FragmentManager.Tx && ((FragmentManager.Tx) transaction).mAddToBackStack;
        int id = transaction.commit();
        mBackStackId = onBackStack ? id : -1;
        return mBackStackId;
    }

    public void showNow(FragmentManager manager, String tag) {
        mDismissed = false;
        mShownByMe = true;
        FragmentTransaction ft = manager.beginTransaction();
        ft.add(this, tag);
        ft.commitNow();
    }

    public void dismiss() { dismissInternal(false); }

    public void dismissAllowingStateLoss() { dismissInternal(true); }

    void dismissInternal(boolean allowStateLoss) {
        if (mDismissed) return;
        mDismissed = true;
        mShownByMe = false;
        if (mDialog != null) {
            mDialog.dismiss();
            mDialog = null;
        }
        mViewDestroyed = true;
        FragmentManager fm = getFragmentManager();
        if (fm == null) return;
        if (mBackStackId >= 0) {
            fm.popBackStack(mBackStackId, FragmentManager.POP_BACK_STACK_INCLUSIVE);
            mBackStackId = -1;
        } else {
            FragmentTransaction ft = fm.beginTransaction();
            ft.remove(this);
            if (allowStateLoss) ft.commitAllowingStateLoss();
            else ft.commit();
        }
    }

    public Dialog getDialog() { return mDialog; }

    public int getTheme() { return mTheme; }

    public void setCancelable(boolean cancelable) {
        mCancelable = cancelable;
        if (mDialog != null) mDialog.setCancelable(cancelable);
    }

    public boolean isCancelable() { return mCancelable; }

    public void setShowsDialog(boolean showsDialog) { mShowsDialog = showsDialog; }

    public boolean getShowsDialog() { return mShowsDialog; }

    @Override
    public void onAttach(Context context) {
        super.onAttach(context);
        if (!mShownByMe) mDismissed = false;
    }

    @Override
    public void onDetach() {
        super.onDetach();
        if (!mShownByMe && !mDismissed) mDismissed = true;
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mShowsDialog = mContainerId == 0;
        if (savedInstanceState != null) {
            mStyle = savedInstanceState.getInt(SAVED_STYLE, STYLE_NORMAL);
            mTheme = savedInstanceState.getInt(SAVED_THEME, 0);
            mCancelable = savedInstanceState.getBoolean(SAVED_CANCELABLE, true);
            mShowsDialog = savedInstanceState.getBoolean(SAVED_SHOWS_DIALOG, mShowsDialog);
            mBackStackId = savedInstanceState.getInt(SAVED_BACK_STACK_ID, -1);
        }
    }

    /** The dialog, made once: by onCreateDialog, then set up for the style. */
    private void makeDialog(Bundle savedInstanceState) {
        if (!mShowsDialog || mDialog != null) return;
        mDialog = onCreateDialog(savedInstanceState);
        if (mDialog != null) setupDialog(mDialog, mStyle);
    }

    @Override
    public LayoutInflater onGetLayoutInflater(Bundle savedInstanceState) {
        if (!mShowsDialog) return super.onGetLayoutInflater(savedInstanceState);
        makeDialog(savedInstanceState);
        if (mDialog != null) return (LayoutInflater) mDialog.getContext().getSystemService(Context.LAYOUT_INFLATER_SERVICE);
        return (LayoutInflater) getActivity().getSystemService(Context.LAYOUT_INFLATER_SERVICE);
    }

    /** @hide Android's own, which subclasses in the platform override; public there too. */
    public void setupDialog(Dialog dialog, int style) {
        switch (style) {
        case STYLE_NO_INPUT:
            dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
            // fall through
        case STYLE_NO_FRAME:
        case STYLE_NO_TITLE:
            dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
            break;
        default:
            break;
        }
    }

    public Dialog onCreateDialog(Bundle savedInstanceState) { return new Dialog(getActivity(), getTheme()); }

    @Override
    public void onCancel(DialogInterface dialog) {}

    @Override
    public void onDismiss(DialogInterface dialog) {
        if (!mViewDestroyed) dismissInternal(true);
    }

    @Override
    public void onActivityCreated(Bundle savedInstanceState) {
        super.onActivityCreated(savedInstanceState);
        if (!mShowsDialog) return;
        makeDialog(savedInstanceState);
        if (mDialog == null) return;
        View view = getView();
        if (view != null) {
            if (view.getParent() != null) throw new IllegalStateException("DialogFragment can not be attached to a container view");
            mDialog.setContentView(view);
        }
        Activity activity = getActivity();
        if (activity != null) mDialog.setOwnerActivity(activity);
        mDialog.setCancelable(mCancelable);
        mDialog.setOnCancelListener(this);
        mDialog.setOnDismissListener(this);
        if (savedInstanceState != null) {
            Bundle dialogState = savedInstanceState.getBundle(SAVED_DIALOG_STATE_TAG);
            if (dialogState != null) mDialog.onRestoreInstanceState(dialogState);
        }
    }

    @Override
    public void onStart() {
        super.onStart();
        if (mDialog != null) {
            mViewDestroyed = false;
            mDialog.show();
        }
    }

    @Override
    public void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        if (mDialog != null) {
            Bundle dialogState = mDialog.onSaveInstanceState();
            if (dialogState != null) outState.putBundle(SAVED_DIALOG_STATE_TAG, dialogState);
        }
        if (mStyle != STYLE_NORMAL) outState.putInt(SAVED_STYLE, mStyle);
        if (mTheme != 0) outState.putInt(SAVED_THEME, mTheme);
        if (!mCancelable) outState.putBoolean(SAVED_CANCELABLE, mCancelable);
        if (!mShowsDialog) outState.putBoolean(SAVED_SHOWS_DIALOG, mShowsDialog);
        if (mBackStackId != -1) outState.putInt(SAVED_BACK_STACK_ID, mBackStackId);
    }

    @Override
    public void onStop() {
        super.onStop();
        if (mDialog != null) mDialog.hide();
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        if (mDialog != null) {
            mViewDestroyed = true;
            mDialog.dismiss();
            mDialog = null;
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        // The original's FragmentManager calls onDestroyView only for a fragment with a view; a dialog made by onCreateDialog alone goes here.
        if (mDialog != null) {
            mViewDestroyed = true;
            mDialog.dismiss();
            mDialog = null;
        }
    }

    @Override
    public void dump(String prefix, java.io.FileDescriptor fd, java.io.PrintWriter writer, String[] args) {
        super.dump(prefix, fd, writer, args);
        writer.print(prefix);
        writer.println("DialogFragment:");
        writer.print(prefix);
        writer.print("  mStyle=");
        writer.print(mStyle);
        writer.print(" mTheme=0x");
        writer.println(Integer.toHexString(mTheme));
        writer.print(prefix);
        writer.print("  mCancelable=");
        writer.print(mCancelable);
        writer.print(" mShowsDialog=");
        writer.print(mShowsDialog);
        writer.print(" mBackStackId=");
        writer.println(mBackStackId);
        writer.print(prefix);
        writer.print("  mDialog=");
        writer.println(mDialog);
        writer.print(prefix);
        writer.print("  mViewDestroyed=");
        writer.print(mViewDestroyed);
        writer.print(" mDismissed=");
        writer.print(mDismissed);
        writer.print(" mShownByMe=");
        writer.println(mShownByMe);
    }
}
