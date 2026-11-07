package megler.test.screen;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

/**
 * The launcher activity, which takes no configuration change itself: its left half red and its right half blue, whatever the
 * surface's size, and a field whose text it typed at its first start -- the state it must find again when it is made again,
 * with the count of its makings and what it retained. Every step of its life is a line of the log.
 */
public class Main extends Activity {
    static final String TAG = App.TAG;
    /* The field's id: a view's state is saved and given back by its id. */
    static final int FIELD = 1001;
    private int count;
    private EditText field;

    /** A half of the screen: a colour, and (when named) what it hears of the configuration and of its size. */
    static class Half extends View {
        private final String name;

        Half(Context c, int color, String name) {
            super(c);
            this.name = name;
            setBackgroundColor(color);
        }

        @Override
        protected void onConfigurationChanged(Configuration c) {
            super.onConfigurationChanged(c);
            if (name != null) Log.i(TAG, name + ".view onConfigurationChanged " + App.describe(c));
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            if (name != null) Log.i(TAG, name + ".view size " + w + "x" + h);
        }
    }

    /** The configuration, the display and the resources as the activity reads them. */
    static String screen(Activity a) {
        Configuration c = a.getResources().getConfiguration();
        DisplayMetrics m = a.getResources().getDisplayMetrics();
        Display d = a.getWindowManager().getDefaultDisplay();
        return App.describe(c) + " rotation=" + d.getRotation() + " metrics=" + m.widthPixels + "x" + m.heightPixels + " res=" + a.getString(R.string.width_class);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        count = state == null ? 1 : state.getInt("count") + 1;
        Object kept = getLastNonConfigurationInstance();
        Log.i(TAG, "Main.onCreate count=" + count + " state=" + (state == null ? "none" : "saved") + " retained=" + (kept == null ? "none" : kept));
        FrameLayout root = new FrameLayout(this);
        LinearLayout halves = new LinearLayout(this);
        halves.setOrientation(LinearLayout.HORIZONTAL);
        halves.addView(new Half(this, 0xFFFF0000, null), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        halves.addView(new Half(this, 0xFF0000FF, "Main.right"), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        root.addView(halves, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        field = new EditText(this);
        field.setId(FIELD);
        root.addView(field, new FrameLayout.LayoutParams(200, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        setContentView(root);
        /* What the user typed, as far as the test goes: only the first one types it, the others have it back from the saved state. */
        if (state == null) field.setText("typed");
        Log.i(TAG, "Main.config " + screen(this));
    }

    @Override
    public Object onRetainNonConfigurationInstance() {
        Log.i(TAG, "Main.onRetain");
        return "kept" + count;
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("count", count);
        Log.i(TAG, "Main.onSaveInstanceState field=" + field.getText());
    }

    @Override
    protected void onRestoreInstanceState(Bundle state) {
        super.onRestoreInstanceState(state);
        Log.i(TAG, "Main.onRestoreInstanceState field=" + field.getText());
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "Main.onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.i(TAG, "Main.onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "Main.onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.i(TAG, "Main.onPause changing=" + isChangingConfigurations());
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "Main.onStop changing=" + isChangingConfigurations());
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "Main.onDestroy changing=" + isChangingConfigurations() + " changes=0x" + Integer.toHexString(getChangingConfigurations()));
    }

    /* Never, as it takes no change itself: the test checks that it is not called. */
    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        Log.i(TAG, "Main.onConfigurationChanged " + App.describe(c));
    }
}
