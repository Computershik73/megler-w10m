package android.text;

import java.util.ArrayList;

/** Editable text with spans that follow the edits (by their point/mark flags), filters applied, and watchers told. */
public class SpannableStringBuilder extends SpannableStringInternal implements CharSequence, GetChars, Spannable, Editable, Appendable {
    private InputFilter[] mFilters = new InputFilter[0];
    public SpannableStringBuilder() { super("", 0, 0); }
    public SpannableStringBuilder(CharSequence s) { super(s, 0, s.length()); }
    public SpannableStringBuilder(CharSequence s, int start, int end) { super(s, start, end); }
    public static SpannableStringBuilder valueOf(CharSequence s) { return s instanceof SpannableStringBuilder ? (SpannableStringBuilder) s : new SpannableStringBuilder(s); }
    public void setFilters(InputFilter[] f) { mFilters = f == null ? new InputFilter[0] : f; }
    public InputFilter[] getFilters() { return mFilters; }
    public CharSequence subSequence(int start, int end) { return new SpannableStringBuilder(this, start, end); }
    public SpannableStringBuilder insert(int where, CharSequence tb, int start, int end) { return replace(where, where, tb, start, end); }
    public SpannableStringBuilder insert(int where, CharSequence tb) { return replace(where, where, tb, 0, tb.length()); }
    public SpannableStringBuilder delete(int start, int end) { return replace(start, end, "", 0, 0); }
    public SpannableStringBuilder append(CharSequence text) { int l = mLen; return replace(l, l, text, 0, text.length()); }
    public SpannableStringBuilder append(CharSequence text, Object what, int flags) { int s = mLen; append(text); setSpan(what, s, mLen, flags); return this; }
    public SpannableStringBuilder append(CharSequence text, int start, int end) { int l = mLen; return replace(l, l, text, start, end); }
    public SpannableStringBuilder append(char c) { return append(String.valueOf(c)); }
    public void clear() { replace(0, mLen, "", 0, 0); }
    public void clearSpans() { for (int i = mSpanCount - 1; i >= 0; i--) { Object w = mSpans[i]; int s = mStarts[i], e = mEnds[i]; removeSpanAt(i); sendSpanRemoved(w, s, e); } }
    public SpannableStringBuilder replace(int start, int end, CharSequence tb) { return replace(start, end, tb, 0, tb.length()); }
    /*
     * As Android's: the range checked first (it was not, and an edit beyond the end moved what it should not); the text
     * watchers found once, where the edit is, and each told before, on and after it -- they were found again after the
     * edit for onTextChanged and afterTextChanged, where the new text is, so that one put on during beforeTextChanged
     * heard the rest of an edit it had not heard begin; and a cursor or selection's end inside the replaced text keeps its
     * place relative to it (a point at its start stays there, see adjust).
     */
    public SpannableStringBuilder replace(int start, int end, CharSequence tb, int tbstart, int tbend) {
        checkRange("replace", start, end);
        for (InputFilter f : mFilters) {
            CharSequence repl = f.filter(tb, tbstart, tbend, this, start, end);
            if (repl != null) { tb = repl; tbstart = 0; tbend = repl.length(); }
        }
        int origLen = end - start, newLen = tbend - tbstart;
        if (origLen == 0 && newLen == 0) return this;
        TextWatcher[] watchers = getSpans(start, start + origLen, TextWatcher.class);
        for (TextWatcher w : watchers) w.beforeTextChanged(this, start, origLen, newLen);
        // the text
        int delta = newLen - origLen;
        if (mLen + delta > mText.length) mText = java.util.Arrays.copyOf(mText, Math.max(mText.length * 2, mLen + delta + 16));
        System.arraycopy(mText, end, mText, end + delta, mLen - end);
        for (int i = 0; i < newLen; i++) mText[start + i] = tb.charAt(tbstart + i);
        mLen += delta;
        // the spans: those after move, those inside shrink, points and marks at the edges follow their flags; those that moved
        // are noted, with where they were, for the span watchers (below)
        boolean adjustSelection = origLen != 0 && newLen != 0;
        boolean watched = false;
        for (int i = 0; i < mSpanCount && !watched; i++) watched = mSpans[i] instanceof SpanWatcher;
        Object[] moved = null;
        int[] was = null;
        int movedCount = 0;
        for (int i = 0; i < mSpanCount; i++) {
            int os = mStarts[i], oe = mEnds[i];
            mStarts[i] = adjust(mStarts[i], start, end, delta, (mFlags[i] & 0xf0) >> 4, true, origLen);
            mEnds[i] = adjust(mEnds[i], start, end, delta, mFlags[i] & 0x0f, false, origLen);
            if (mEnds[i] < mStarts[i]) mEnds[i] = mStarts[i];
            if (adjustSelection && os > start && os < end && (mSpans[i] == Selection.SELECTION_START || mSpans[i] == Selection.SELECTION_END))
                mStarts[i] = mEnds[i] = start + (int) ((long) (os - start) * newLen / origLen);
            if (watched && (mStarts[i] != os || mEnds[i] != oe)) {
                if (moved == null) { moved = new Object[mSpanCount]; was = new int[3 * mSpanCount]; }
                moved[movedCount] = mSpans[i]; was[3 * movedCount] = os; was[3 * movedCount + 1] = oe; was[3 * movedCount + 2] = i; movedCount++;
            }
        }
        // spans of a deleted range that are now empty and exclusive go away
        for (int i = mSpanCount - 1; i >= 0; i--) {
            if (mStarts[i] == mEnds[i] && origLen > 0 && (mFlags[i] & SPAN_POINT_MARK_MASK) == SPAN_EXCLUSIVE_EXCLUSIVE && mStarts[i] == start && !(mSpans[i] instanceof NoCopySpan)) {
                Object w = mSpans[i]; int s = mStarts[i], e2 = mEnds[i]; removeSpanAt(i); sendSpanRemoved(w, s, e2);
            }
        }
        if (tb instanceof Spanned) {
            Spanned sp = (Spanned) tb;
            Object[] spans = sp.getSpans(tbstart, tbend, Object.class);
            for (Object o : spans) {
                if (o instanceof NoCopySpan) continue;
                int st = Math.max(sp.getSpanStart(o), tbstart) - tbstart + start, en = Math.min(sp.getSpanEnd(o), tbend) - tbstart + start;
                if (indexOfSpan(o) < 0) setSpan(o, st, en, sp.getSpanFlags(o));
            }
        }
        for (TextWatcher w : watchers) w.onTextChanged(this, start, origLen, newLen);
        for (TextWatcher w : watchers) w.afterTextChanged(this);
        sendMoved(moved, was, movedCount);
        return this;
    }
    /*
     * The span watchers are told of each span the edit moved, after the text watchers, as Android's sendToSpanWatchers tells them:
     * where it was and where it is now, if it is still there. A selection is two such spans, and a TextView learns from this that
     * its cursor moved with the text typed (onSelectionChanged). Nothing was told: the Google app's Gemini field
     * (RobinTextInputEditText, 17.6.63), which keeps its cursor as onSelectionChanged says, kept it where the field was
     * touched while the user typed.
     */
    private void sendMoved(Object[] moved, int[] was, int count) {
        if (count == 0) return;
        // (The watchers once, in getSpans's order; each told of the spans whose range, old or new, meets its own, as
        // getSpans(min, max) would find it. A long text with many spans moves them all, and asking getSpans for each was
        // the square of their number.)
        SpanWatcher[] all = getSpans(0, mLen, SpanWatcher.class);
        int[] ws = new int[all.length], we = new int[all.length];
        for (int j = 0; j < all.length; j++) { ws[j] = getSpanStart(all[j]); we[j] = getSpanEnd(all[j]); }
        for (int k = 0; k < count; k++) {
            Object what = moved[k];
            int i = was[3 * k + 2];
            if (i >= mSpanCount || mSpans[i] != what) i = indexOfSpan(what);
            if (i < 0) continue;
            int os = was[3 * k], oe = was[3 * k + 1], ns = mStarts[i], ne = mEnds[i];
            if (os == ns && oe == ne) continue;
            int qs = Math.min(os, ns), qe = Math.min(Math.max(oe, ne), mLen);
            for (int j = 0; j < all.length; j++) {
                int s = ws[j], e = we[j];
                if (s > qe || e < qs || (s != e && qs != qe && (s == qe || e == qs))) continue;
                all[j].onSpanChanged(this, what, os, oe, ns, ne);
            }
        }
    }
    /**
     * A span edge after replacing [start, end) with delta more characters: 1 mark (stays left), 2 point (moves right). Inside
     * the replaced text a point goes to the end of the new text, but for one at its very start where there is new text,
     * which stays, as Android's updatedIntervalBound keeps it: an input method's commitText(text, 0) over a selection puts
     * the cursor at the selection's start before the text goes in, for the text to be after the cursor, and the cursor
     * went to its end.
     */
    private static int adjust(int pos, int start, int end, int delta, int kind, boolean isStart, int origLen) {
        if (pos > end) return pos + delta;
        if (pos < start) return pos;
        if (pos == start && origLen == 0) {
            // an insertion at the edge: a point moves with the text, a mark stays
            return kind == 2 ? pos + delta : pos;
        }
        if (pos == end && pos != start) return pos + delta;
        // inside the replaced range
        int newLen = Math.max(0, (end - start) + delta);
        if (kind == 2) return pos > start || newLen == 0 ? start + newLen : start;
        return start;
    }
    private void checkRange(String op, int start, int end) {
        if (end < start) throw new IndexOutOfBoundsException(op + " (" + start + " ... " + end + ") has end before start");
        if (start > mLen || end > mLen) throw new IndexOutOfBoundsException(op + " (" + start + " ... " + end + ") ends beyond length " + mLen);
        if (start < 0 || end < 0) throw new IndexOutOfBoundsException(op + " (" + start + " ... " + end + ") starts before 0");
    }
    public void setSpan(Object what, int start, int end, int flags) {
        if (start < 0 || end > mLen || start > end) throw new IndexOutOfBoundsException("setSpan (" + start + " ... " + end + ") ends beyond length " + mLen);
        int i = indexOfSpan(what);
        if (i >= 0) {
            int os = mStarts[i], oe = mEnds[i];
            mStarts[i] = start; mEnds[i] = end; mFlags[i] = flags;
            for (SpanWatcher w : getSpans(Math.min(os, start), Math.max(oe, end), SpanWatcher.class)) w.onSpanChanged(this, what, os, oe, start, end);
            return;
        }
        addSpan(what, start, end, flags);
        if (!(what instanceof SpanWatcher)) for (SpanWatcher w : getSpans(start, end, SpanWatcher.class)) w.onSpanAdded(this, what, start, end);
    }
    public void removeSpan(Object what) { int i = indexOfSpan(what); if (i >= 0) { int s = mStarts[i], e = mEnds[i]; removeSpanAt(i); sendSpanRemoved(what, s, e); } }
    public void removeSpan(Object what, int flags) { removeSpan(what); }
    private void sendSpanRemoved(Object what, int s, int e) { if (what instanceof SpanWatcher) return; for (SpanWatcher w : getSpans(s, e, SpanWatcher.class)) w.onSpanRemoved(this, what, s, e); }
    public int getTextWatcherDepth() { return 0; }
    public String substring(int start, int end) { return new String(mText, start, end - start); }
    public void drawText(android.graphics.Canvas c, int start, int end, float x, float y, android.graphics.Paint p) { c.drawText(toString(), start, end, x, y, p); }
    public float getTextRunAdvances(int s, int e, int cs, int ce, boolean rtl, float[] adv, int ai, android.graphics.Paint p) { return p.measureText(toString(), s, e); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public java.lang.Object[] getSpans(int p0, int p1, java.lang.Class p2, boolean p3) { return null; }
    public int getTextRunCursor(int p0, int p1, int p2, int p3, int p4, android.graphics.Paint p5) { return 0; }
    public int getTextRunCursor(int p0, int p1, boolean p2, int p3, int p4, android.graphics.Paint p5) { return 0; }
    public int getTextWidths(int p0, int p1, float[] p2, android.graphics.Paint p3) { return 0; }
    public float measureText(int p0, int p1, android.graphics.Paint p2) { return 0f; }
    // ---- end of generated members
}
