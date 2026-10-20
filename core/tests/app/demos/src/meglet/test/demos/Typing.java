package meglet.test.demos;

import android.app.Activity;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Four text fields, one of each kind a keyboard is told apart by: a line of text whose Enter is "Done", a number, a password, and
 * a note of several lines. Every change of a field's text and every editor action is a line of the log, with line ends written
 * as "|" so that a line of the log stays one.
 */
public class Typing extends Activity {
    private EditText name, number, secret, note;

    private static String place(android.view.View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    private EditText field(LinearLayout root, final String tag, int inputType, int imeOptions, int heightPx) {
        final EditText e = new EditText(this);
        e.setInputType(inputType);
        if (imeOptions != 0) e.setImeOptions(imeOptions);
        e.setTextColor(0xFF102030);
        e.setTextSize(18);
        e.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                Log.i("DemoTest", "typing: " + tag + "=\"" + s.toString().replace('\n', '|') + "\" cursor=" + e.getSelectionStart());
            }
        });
        e.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                Log.i("DemoTest", "typing: action " + tag + " " + actionId);
                return false;
            }
        });
        root.addView(e, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, heightPx));
        return e;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFF0F0F0);
        name = field(root, "name", InputType.TYPE_CLASS_TEXT, EditorInfo.IME_ACTION_DONE, 80);
        number = field(root, "number", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL, EditorInfo.IME_ACTION_NEXT, 80);
        secret = field(root, "secret", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, 0, 80);
        note = field(root, "note", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES, 0, 200);
        setContentView(root);
        Log.i("DemoTest", "typing: created");
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) Log.i("DemoTest", "typing: layout name=" + place(name) + " number=" + place(number) + " secret=" + place(secret) + " note=" + place(note));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DemoTest", "typing: destroyed name=\"" + name.getText() + "\"");
    }
}
