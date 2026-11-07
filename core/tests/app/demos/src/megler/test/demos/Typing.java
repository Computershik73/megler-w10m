package megler.test.demos;

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

    /*
     * A field that keeps its cursor out of a word, as WhatsApp's MentionableEntry keeps it out of a mention: its
     * onSelectionChanged sets the selection again every time, to the very ends it was told when they are outside the word.
     * Android leaves a selection that does not move alone and tells the field once a change is whole, so this ends; Megler
     * told it of each end, and of the same ends again, until the stack ran out. It gives up after 50 calls, for the test.
     */
    static final class Guarded extends EditText {
        int calls;
        String seen = "";

        Guarded(android.content.Context c) {
            super(c);
        }

        private static int outside(int p) {
            return p > 6 && p < 11 ? 11 : p;               /* the word "@Anna", 6 to 11, as one */
        }

        @Override
        protected void onSelectionChanged(int start, int end) {
            super.onSelectionChanged(start, end);
            if (seen == null || getText() == null || ++calls > 50) return;     /* (a TextView's constructor may call it) */
            seen += " " + start + "," + end;
            setSelection(outside(start), outside(end));
        }
    }

    /* The cursor put in and out of the word: "selection calls=3 3,3 8,8 11,11 at 11,11" is Android's answer. */
    private void selectionTold() {
        Guarded g = new Guarded(this);
        g.setText("Hello @Anna, see you");
        g.calls = 0;
        g.seen = "";
        g.setSelection(3);
        g.setSelection(3);
        g.setSelection(8, 8);
        Log.i("DemoTest", "typing: selection calls=" + g.calls + g.seen + " at " + g.getSelectionStart() + "," + g.getSelectionEnd());
    }

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
        selectionTold();
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
