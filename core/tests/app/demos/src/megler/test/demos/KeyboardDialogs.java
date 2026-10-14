package megler.test.demos;

import android.app.Activity;
import android.app.Dialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Dialogs with a text field, over which the keyboard comes up (core/tests/hk_java_tests.c, keyboard_and_dialogs), one after
 * the other as each is closed:
 *  - "top": its field at its top, and it asks to be resized (SOFT_INPUT_ADJUST_RESIZE);
 *  - "bottom": its field at the bottom of a tall dialog, and it asks to be panned (SOFT_INPUT_ADJUST_PAN);
 *  - "scroll": its field in a ScrollView, and it says SOFT_INPUT_ADJUST_UNSPECIFIED, which Android decides as resize because
 *    of the ScrollView;
 *  - "theme": the same, and it says nothing itself: a dialog's theme (Theme.Material.Dialog) says adjustPan.
 * Each says in the log where its field and its window are on the screen whenever that changes, before every frame, and
 * the softInputMode its window has.
 */
public class KeyboardDialogs extends Activity {
    private static final String[] TAGS = { "top", "bottom", "scroll", "theme" };
    private int which = -1;

    private static void say(String what) {
        Log.i("DemoTest", what);
    }

    private static String place(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView t = new TextView(this);
        t.setText("Dialogs and the keyboard");
        setContentView(t);
        say("keyboard: created");
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus && which < 0) next();
    }

    /* The next dialog, or the word that all have been shown. */
    private void next() {
        if (which + 1 >= TAGS.length) {
            say("keyboard: all shown");
            return;
        }
        which++;
        show();
    }

    private void show() {
        final String tag = TAGS[which];
        final Dialog d = new Dialog(this);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        final EditText field = new EditText(this);
        field.setTextSize(18);
        field.setTextColor(0xFF102030);
        View filler = new View(this);
        filler.setBackgroundColor(0xFFDDEEFF);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackgroundColor(0xFFFFFFFF);
        if (which == 0) {
            box.addView(field, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 60));
            box.addView(filler, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 160));
            d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        } else if (which == 1) {
            box.addView(filler, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 400));
            box.addView(field, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 60));
            d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
        } else {
            if (which == 2) d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED);
            ScrollView scroll = new ScrollView(this);
            LinearLayout inner = new LinearLayout(this);
            inner.setOrientation(LinearLayout.VERTICAL);
            inner.addView(field, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 60));
            inner.addView(filler, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 160));
            scroll.addView(inner, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            box.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        d.setContentView(box);
        d.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialog) {
                say("keyboard: " + tag + " closed");
                getWindow().getDecorView().post(new Runnable() {
                    @Override
                    public void run() {
                        next();
                    }
                });
            }
        });
        d.show();
        final View decor = d.getWindow().getDecorView();
        decor.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            private String last = "";

            @Override
            public boolean onPreDraw() {
                String now = "field=" + place(field) + " window=" + place(decor) + " mode=0x" + Integer.toHexString(d.getWindow().getAttributes().softInputMode);
                if (!now.equals(last)) {
                    last = now;
                    say("keyboard: " + tag + " " + now);
                }
                return true;
            }
        });
    }
}
