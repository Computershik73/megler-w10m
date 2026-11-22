package android.view.inputmethod;

import android.os.Bundle;
import android.text.Editable;
import android.text.Selection;
import android.view.KeyEvent;
import android.view.View;

/**
 * An input connection over an Editable: text committed at the cursor, deletes around it, keys sent to the view.
 *
 * The text being composed is marked as Android marks it, by a span of its own on the Editable (COMPOSING, exclusive at both
 * ends, flagged SPAN_COMPOSING), which moves with the edits the field's own watchers make. It was two numbers kept here:
 * a watcher that changed the text before them left them where the composing text no longer was, and the next
 * setComposingText replaced the wrong characters. And deleteSurroundingText deletes as Android's does: around the selection
 * and the composing text both, what is before first and then what is after, the composing text kept. It deleted around the
 * selection alone, what is after first, and forgot the composing text.
 */
public class BaseInputConnection implements InputConnection {
    /* What marks the composing text: no copy of the text takes it along. */
    private static final class ComposingText implements android.text.NoCopySpan {}
    private static final Object COMPOSING = new ComposingText();
    protected final InputMethodManager mIMM;
    final View mTargetView;
    final boolean mFallbackMode;
    private Editable mDefaultEditable;
    private int mBatch;
    public BaseInputConnection(View targetView, boolean fullEditor) {
        mIMM = (InputMethodManager) targetView.getContext().getSystemService(android.content.Context.INPUT_METHOD_SERVICE);
        mTargetView = targetView; mFallbackMode = !fullEditor;
    }
    public Editable getEditable() {
        if (mDefaultEditable == null) { mDefaultEditable = Editable.Factory.getInstance().newEditable(""); Selection.setSelection(mDefaultEditable, 0); }
        return mDefaultEditable;
    }
    public static int getComposingSpanStart(android.text.Spannable t) { return t.getSpanStart(COMPOSING); }
    public static int getComposingSpanEnd(android.text.Spannable t) { return t.getSpanEnd(COMPOSING); }
    /** As Android's: the composing mark taken off, and every span flagged SPAN_COMPOSING (an input method's underline). */
    public static void removeComposingSpans(android.text.Spannable t) {
        t.removeSpan(COMPOSING);
        Object[] sps = t.getSpans(0, t.length(), Object.class);
        for (int i = sps.length - 1; i >= 0; i--) if ((t.getSpanFlags(sps[i]) & android.text.Spanned.SPAN_COMPOSING) != 0) t.removeSpan(sps[i]);
    }
    public static void setComposingSpans(android.text.Spannable t) { setComposingSpans(t, 0, t.length()); }
    /** As Android's: the spans of the range made composing ones (exclusive at both ends), and the range marked composing. */
    public static void setComposingSpans(android.text.Spannable t, int start, int end) {
        Object[] sps = t.getSpans(start, end, Object.class);
        for (int i = sps.length - 1; i >= 0; i--) {
            Object o = sps[i];
            if (o == COMPOSING) { t.removeSpan(o); continue; }
            int fl = t.getSpanFlags(o);
            if ((fl & (android.text.Spanned.SPAN_COMPOSING | android.text.Spanned.SPAN_POINT_MARK_MASK)) != (android.text.Spanned.SPAN_COMPOSING | android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE))
                t.setSpan(o, t.getSpanStart(o), t.getSpanEnd(o), (fl & ~android.text.Spanned.SPAN_POINT_MARK_MASK) | android.text.Spanned.SPAN_COMPOSING | android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        t.setSpan(COMPOSING, start, end, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | android.text.Spanned.SPAN_COMPOSING);
    }
    public boolean beginBatchEdit() { mBatch++; return true; }
    public boolean endBatchEdit() { if (mBatch > 0) mBatch--; return mBatch > 0; }
    public void closeConnection() { finishComposingText(); }
    public boolean clearMetaKeyStates(int s) { return false; }
    public boolean commitCompletion(CompletionInfo t) { return false; }
    public boolean commitCorrection(CorrectionInfo i) { return false; }
    public boolean commitText(CharSequence text, int pos) { replace(text, pos, false); return true; }
    /* As Android's: before the selection and the composing text, then after them, the composing text left alone. */
    public boolean deleteSurroundingText(int before, int after) {
        Editable c = getEditable(); if (c == null) return false;
        beginBatchEdit();
        int a = Selection.getSelectionStart(c), b = Selection.getSelectionEnd(c);
        if (a > b) { int t = a; a = b; b = t; }
        int ca = getComposingSpanStart(c), cb = getComposingSpanEnd(c);
        if (cb < ca) { int t = ca; ca = cb; cb = t; }
        if (ca != -1 && cb != -1) { if (ca < a) a = ca; if (cb > b) b = cb; }
        int deleted = 0;
        if (before > 0) {
            int start = Math.max(0, a - before);
            if (a >= 0 && a - start > 0) { c.delete(start, a); deleted = a - start; }
        }
        if (after > 0) {
            b -= deleted;
            int end = Math.min(c.length(), b + after);
            if (b >= 0 && end - b > 0) c.delete(b, end);
        }
        endBatchEdit();
        return true;
    }
    public boolean deleteSurroundingTextInCodePoints(int before, int after) { return deleteSurroundingText(before, after); }
    public boolean finishComposingText() { Editable c = getEditable(); if (c != null) removeComposingSpans(c); return true; }
    public int getCursorCapsMode(int req) { Editable c = getEditable(); if (c == null) return 0; int a = Selection.getSelectionStart(c); return android.text.TextUtils.getCapsMode(c, Math.max(0, a), req); }
    public ExtractedText getExtractedText(ExtractedTextRequest r, int f) { return null; }
    /* These three as Android's: with the text's spans where GET_TEXT_WITH_STYLES asks for them. */
    public CharSequence getTextBeforeCursor(int n, int flags) {
        Editable c = getEditable(); if (c == null) return null;
        int a = Math.min(Selection.getSelectionStart(c), Selection.getSelectionEnd(c));
        if (a <= 0) return "";
        if (n > a) n = a;
        return text(c, a - n, a, flags);
    }
    public CharSequence getSelectedText(int flags) {
        Editable c = getEditable(); if (c == null) return null;
        int a = Selection.getSelectionStart(c), b = Selection.getSelectionEnd(c);
        if (a == b || a < 0) return null;
        return text(c, Math.min(a, b), Math.max(a, b), flags);
    }
    public CharSequence getTextAfterCursor(int n, int flags) {
        Editable c = getEditable(); if (c == null) return null;
        int b = Math.max(Selection.getSelectionStart(c), Selection.getSelectionEnd(c));
        if (b < 0) b = 0;
        if (b + n > c.length()) n = c.length() - b;
        return text(c, b, b + n, flags);
    }
    private static CharSequence text(Editable c, int start, int end, int flags) {
        return (flags & GET_TEXT_WITH_STYLES) != 0 ? c.subSequence(start, end) : c.subSequence(start, end).toString();
    }
    public boolean performEditorAction(int action) {
        long t = android.os.SystemClock.uptimeMillis();
        sendKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER, 0));
        sendKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER, 0));
        return true;
    }
    public boolean performContextMenuAction(int id) { return false; }
    public boolean performPrivateCommand(String a, Bundle d) { return false; }
    public boolean requestCursorUpdates(int m) { return false; }
    public android.os.Handler getHandler() { return null; }
    public boolean setComposingText(CharSequence text, int pos) { replace(text, pos, true); return true; }
    /* As Android's: the range clipped to the text, and marked as the text being composed. */
    public boolean setComposingRegion(int s, int e) {
        Editable c = getEditable(); if (c == null) return true;
        beginBatchEdit();
        removeComposingSpans(c);
        int a = Math.min(s, e), b = Math.max(s, e), len = c.length();
        a = Math.max(0, Math.min(a, len)); b = Math.max(0, Math.min(b, len));
        c.setSpan(COMPOSING, a, b, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | android.text.Spanned.SPAN_COMPOSING);
        endBatchEdit();
        return true;
    }
    public boolean setSelection(int s, int e) { Editable c = getEditable(); if (c == null) return false; int len = c.length(); if (s > len || e > len || s < 0 || e < 0) return true; Selection.setSelection(c, s, e); return true; }
    public boolean sendKeyEvent(KeyEvent e) { mTargetView.dispatchKeyEvent(e); return false; }
    public boolean reportFullscreenMode(boolean e) { return true; }
    public boolean commitContent(InputContentInfo i, int f, Bundle o) { return false; }
    /*
     * The text in place of the composing text, or of the selection, as Android's replaceText: the cursor is put where it is to be
     * before the text goes in, at the end of what is replaced (or newCursor away), so that it moves with the edit -- and a
     * TextWatcher that rewrites the text and puts the cursor itself has the last word. It was put after, at the replaced
     * range's start plus the text's length: WhatsApp's phone number field, which adds dashes as one types, had its cursor put
     * back before the dash it had just added, and the next digit went in there, the digits out of order.
     */
    private void replace(CharSequence text, int newCursor, boolean composing) {
        Editable c = getEditable(); if (c == null) return;
        beginBatchEdit();
        int a = getComposingSpanStart(c), b = getComposingSpanEnd(c);
        if (b < a) { int t = a; a = b; b = t; }
        if (a != -1 && b != -1) removeComposingSpans(c);
        else { a = Selection.getSelectionStart(c); b = Selection.getSelectionEnd(c); if (a < 0) a = 0; if (b < 0) b = 0; if (b < a) { int t = a; a = b; b = t; } }
        if (composing) {
            if (!(text instanceof android.text.Spannable)) text = new android.text.SpannableStringBuilder(text);
            setComposingSpans((android.text.Spannable) text);
        }
        int cursor = newCursor > 0 ? b + newCursor - 1 : a + newCursor;
        if (cursor < 0) cursor = 0; if (cursor > c.length()) cursor = c.length();
        Selection.setSelection(c, cursor);
        int before = c.length();
        c.replace(a, b, text);
        // The composing mark put on where the text went in, as long as what the filters let in: the text carried it, but an
        // edit here takes no NoCopySpan along, which Android's does.
        if (composing) c.setSpan(COMPOSING, a, Math.max(a, Math.min(c.length(), a + c.length() - before + (b - a))), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | android.text.Spanned.SPAN_COMPOSING);
        endBatchEdit();
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public void endComposingRegionEditInternal() {}
    public boolean replaceText(int p0, int p1, java.lang.CharSequence p2, int p3, android.view.inputmethod.TextAttribute p4) { return false; }
    // ---- end of generated members
}
