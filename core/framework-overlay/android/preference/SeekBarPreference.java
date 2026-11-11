// A class the original (core/framework/src/android/preference) did not have: Android's preference whose value is a
// number chosen on a bar in its row. Android hides the class from apps' code and still makes it for a preferences file that
// names it, as any other of the package:
//     <SeekBarPreference android:key="quality" android:max="100" android:defaultValue="60"/>
// spacedesk's compression settings are such a file; on a Lumia 950 (0.1.0.679) opening them ended the app with
// "InflateException: preferences 7f150003: java.lang.ClassNotFoundException: android.preference.SeekBarPreference".
//
// It is Android 9's class on this package's Preference: the value is an int from 0 to android:max (0 where the file gives
// no maximum, as Android's), kept in the preferences under the key; a default is written there when the preference joins
// a screen that has no value yet. A finger's drag is one change, told to the change listener when the finger lifts (a
// refusal puts the bar back); a change without a drag (a key) is told at once. The row is Preference's own, with the bar
// under the title and the summary -- or the bar of the app's android:layout, if that has one. A summary is not shown
// (getSummary is null, as Android's). Not done: the value kept across a recreated activity for a preference that is not
// persistent (this package saves no hierarchy state).
package android.preference;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.widget.LinearLayout;
import android.widget.SeekBar;

/** A preference that is a number, chosen on a bar in its row. */
@Deprecated
public class SeekBarPreference extends Preference implements SeekBar.OnSeekBarChangeListener {
    private int mProgress;
    private int mMax;
    private boolean mTrackingTouch;

    public SeekBarPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (attrs != null) {
            TypedArray a = context.obtainStyledAttributes(attrs, new int[] { android.R.attr.max }, defStyleAttr, defStyleRes);
            setMax(a.getInt(0, mMax));
            a.recycle();
        }
    }

    public SeekBarPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        this(context, attrs, defStyleAttr, 0);
    }

    public SeekBarPreference(Context context, AttributeSet attrs) {
        this(context, attrs, 0, 0);
    }

    public SeekBarPreference(Context context) {
        this(context, null, 0, 0);
    }

    /** The first bar of a row, wherever its layout has it. */
    private static SeekBar bar(View v) {
        if (v instanceof SeekBar) return (SeekBar) v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                SeekBar b = bar(g.getChildAt(i));
                if (b != null) return b;
            }
        }
        return null;
    }

    @Override
    protected View onCreateView(ViewGroup parent) {
        View row = super.onCreateView(parent);
        if (bar(row) == null) {
            View title = row.findViewById(android.R.id.title);
            ViewParent texts = title != null ? title.getParent() : null;
            if (texts instanceof LinearLayout && ((LinearLayout) texts).getOrientation() == LinearLayout.VERTICAL) {
                ((LinearLayout) texts).addView(new SeekBar(getContext()),
                        new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
        }
        return row;
    }

    @Override
    protected void onBindView(View view) {
        super.onBindView(view);
        SeekBar bar = bar(view);
        if (bar == null) return;
        bar.setOnSeekBarChangeListener(this);
        bar.setMax(mMax);
        bar.setProgress(mProgress);
        bar.setEnabled(isEnabled());
    }

    @Override
    public CharSequence getSummary() {
        return null;
    }

    @Override
    protected void onSetInitialValue(boolean restoreValue, Object defaultValue) {
        setProgress(restoreValue ? getPersistedInt(mProgress) : (Integer) defaultValue);
    }

    @Override
    protected Object onGetDefaultValue(TypedArray a, int index) {
        return a.getInt(index, 0);
    }

    @Override
    public boolean onKey(View v, int keyCode, KeyEvent event) {
        if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
        SeekBar bar = bar(v);
        return bar != null && bar.onKeyDown(keyCode, event);
    }

    public void setMax(int max) {
        if (max != mMax) {
            mMax = max;
            notifyChanged();
        }
    }

    public void setProgress(int progress) {
        setProgress(progress, true);
    }

    private void setProgress(int progress, boolean notifyChanged) {
        if (progress > mMax) progress = mMax;
        if (progress < 0) progress = 0;
        if (progress != mProgress) {
            mProgress = progress;
            persistInt(progress);
            if (notifyChanged) notifyChanged();
        }
    }

    public int getProgress() {
        return mProgress;
    }

    /** The bar's value becomes the preference's if the change listener takes it; if not, the bar goes back. */
    void syncProgress(SeekBar seekBar) {
        int progress = seekBar.getProgress();
        if (progress != mProgress) {
            if (callChangeListener(progress)) setProgress(progress, false);
            else seekBar.setProgress(mProgress);
        }
    }

    @Override
    public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
        if (fromUser && !mTrackingTouch) syncProgress(seekBar);
    }

    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {
        mTrackingTouch = true;
    }

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {
        mTrackingTouch = false;
        if (seekBar.getProgress() != mProgress) syncProgress(seekBar);
    }
}
