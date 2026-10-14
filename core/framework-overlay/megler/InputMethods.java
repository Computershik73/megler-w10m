// megler-w10m overlay of Megler/src/java-framework/src/megler/InputMethods.java (tools/build-framework.sh).
// It differs in what deliverPending writes in the log. What it does with the keyboard's text is the original's, moved
// unchanged into apply().
//
// Why. On the phone typed text has never been seen to reach a field, and the shell's log can only say that the text left
// the shell (megler_text_commit, megler_text_key). Once per run the first thing the keyboard sends now says where it went:
// - which view has the focus (its class, or none), and whether it gave an input connection;
// - for a text view, its text's length before and after, so it shows whether the commit was applied. The text itself is
//   never written.
// After that a line is written only when something changes: when text arrives while no view has the focus, or while the
// focused view has no input connection, and when text reaches a field again.
package megler;

import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * The soft keyboard: the host's keyboard comes up for a focused view that edits text, and what is typed goes to that view's input
 * connection (text committed, backspace as a delete before the cursor, return as the editor action). Text arrives through
 * Native.pollText as a list of strings: "t<text>" commit, "d" delete one, "e" the editor action.
 */
public final class InputMethods {
    private static View sFocused;
    private static InputConnection sConn;
    private static EditorInfo sInfo;
    private static boolean sShown;
    /* What the log last said of where the keyboard's text goes: 0 nothing yet, 1 into a view's input connection, 2 nowhere for want of a focused view, 3 nowhere for want of an input connection. */
    private static int sSaid;
    private InputMethods() {}
    public static View focused() { return sFocused; }
    public static boolean shown() { return sShown; }
    public static void focusChanged(View old, View now) {
        if (now != null && now.onCheckIsTextEditor()) { start(now); }
        else if (old != null && old == sFocused) { finish(); hide(); }
    }
    private static void start(View v) {
        if (sFocused == v && sConn != null) return;
        if (sConn != null) sConn.finishComposingText();
        sFocused = v;
        sInfo = new EditorInfo();
        sConn = v.onCreateInputConnection(sInfo);
    }
    private static void finish() { if (sConn != null) sConn.finishComposingText(); sConn = null; sFocused = null; }
    public static void restart(View v) { if (v == sFocused) { sConn = null; start(v); if (sShown) Native.showKeyboard(true, sInfo.inputType, sInfo.imeOptions); } }
    public static boolean show(View v) {
        if (v == null) return false;
        if (v != sFocused) start(v);
        if (sConn == null) return false;
        sShown = true;
        Native.showKeyboard(true, sInfo != null ? sInfo.inputType : 1, sInfo != null ? sInfo.imeOptions : 0);
        return true;
    }
    public static boolean hide() { boolean was = sShown; sShown = false; Native.showKeyboard(false, 0, 0); return was; }
    /** The host has text for us (an event in the input queue said so). */
    public static void deliverPending() {
        String[] items = Native.pollText();
        if (items == null) return;
        for (String it : items) {
            if (it.isEmpty()) continue;
            char k = it.charAt(0);
            if (k == 'h') { sShown = false; continue; }           /* the host closed the keyboard */
            int state = sConn != null ? 1 : sFocused == null ? 2 : 3;
            View target = sFocused;
            int before = state == 1 && state != sSaid ? length(target) : -1;
            boolean took = apply(k, it);
            if (state != sSaid) tell(state, k, it, target, before, took);
        }
    }
    /** One thing the keyboard sent, as the original's deliverPending handles it; false when a commit was refused. */
    private static boolean apply(char k, String it) {
        if (sConn == null) {
            if (sFocused != null) { long t = android.os.SystemClock.uptimeMillis(); if (k == 'd') { sFocused.dispatchKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL, 0)); sFocused.dispatchKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL, 0)); } }
            return false;
        }
        switch (k) {
        case 't': return sConn.commitText(it.substring(1), 1);
        case 'd': {
            CharSequence sel = sConn.getSelectedText(0);
            if (sel != null && sel.length() > 0) sConn.commitText("", 1);
            else {
                CharSequence before = sConn.getTextBeforeCursor(2, 0);
                int n = before != null && before.length() == 2 && Character.isSurrogatePair(before.charAt(0), before.charAt(1)) ? 2 : 1;
                if (before == null || before.length() == 0) { long t = android.os.SystemClock.uptimeMillis(); sConn.sendKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL, 0)); sConn.sendKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL, 0)); }
                else sConn.deleteSurroundingText(n, 0);
            }
            return true;
        }
        case 'e': {
            int action = sInfo != null ? sInfo.imeOptions & EditorInfo.IME_MASK_ACTION : EditorInfo.IME_ACTION_DONE;
            boolean multi = sInfo != null && (sInfo.inputType & android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0 && (sInfo.imeOptions & EditorInfo.IME_FLAG_NO_ENTER_ACTION) == 0;
            if (multi) sConn.commitText("\n", 1);
            else sConn.performEditorAction(action == EditorInfo.IME_ACTION_UNSPECIFIED ? EditorInfo.IME_ACTION_DONE : action);
            return true;
        }
        }
        return true;
    }
    /** A line in the log: where what the keyboard sent went, the first time and whenever that changes (see the header). */
    private static void tell(int state, char k, String it, View target, int before, boolean took) {
        String what = k == 't' ? "typed text (" + count(it.length() - 1, "character") + ")" : k == 'd' ? "a Backspace" : "an Enter";
        String lead = sSaid == 0 ? "the first thing the keyboard sent, " + what + "," : what;
        View keys = ViewRoot.keyFocus();
        String window = keys == target ? "" : "; the window's focused view is " + (keys == null ? "none" : keys.getClass().getName());
        String line;
        if (state == 2) {
            line = lead + " finds no view with the focus: it goes nowhere" + window;
        } else if (state == 3) {
            line = lead + " finds " + target.getClass().getName() + " with the focus but without an input connection: "
                   + (k == 'd' ? "it goes to the view as a Delete key" : "it goes nowhere") + window;
        } else {
            int after = length(target);
            line = lead + (sSaid == 0 ? " went" : " now goes") + " to " + target.getClass().getName() + ", which has the focus and an input connection"
                   + (k == 't' && !took ? ", and the connection refused it" : "")
                   + (before >= 0 && after >= 0 ? "; its text was " + count(before, "character") + " long and is " + after + " now" : "; it is not a TextView, so its text is not measured")
                   + window;
        }
        sSaid = state;
        android.util.Log.i("Megler", "keyboard: " + line);
    }
    private static int length(View v) { return v instanceof android.widget.TextView ? ((android.widget.TextView) v).length() : -1; }
    private static String count(int n, String what) { return n + " " + what + (n == 1 ? "" : "s"); }
    /** A hardware key with a text view focused: printable keys become text there. */
    public static boolean key(KeyEvent e) { return false; }
}
