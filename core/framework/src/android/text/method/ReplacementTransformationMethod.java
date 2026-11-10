package android.text.method;

import android.graphics.Rect;
import android.text.Editable;
import android.text.GetChars;
import android.text.Spannable;
import android.text.Spanned;
import android.text.SpannedString;
import android.text.TextUtils;
import android.view.View;

/**
 * Text shown with some of its characters replaced by others (SingleLineTransformationMethod: a line break by a space), as
 * Android's: the text itself where it has none of them and cannot change; else the text with them replaced and its spans
 * kept -- a copy for text that cannot change, a view that follows it for text that can (Spannable, an Editable).
 *
 * The framework's own gave a String in every case, so a TextView that is android:singleLine (its transformation is
 * SingleLineTransformationMethod) drew its text without a span: Transit hides the markup of a search result's title
 * ("<color|1e212d/f0f0f1>Choose on map", a light theme's colour and a dark one's) under ReplacementSpans of no width and
 * colours the rest by a span of its own, and its single-line rows showed the markup in the view's own colour (0.1.0.684).
 */
public abstract class ReplacementTransformationMethod implements TransformationMethod {
    /** The characters replaced. */
    protected abstract char[] getOriginal();

    /** What each of getOriginal's is replaced by, in the same order. */
    protected abstract char[] getReplacement();

    public CharSequence getTransformation(CharSequence source, View v) {
        char[] original = getOriginal(), replacement = getReplacement();
        if (!(source instanceof Editable)) {
            // Text that will not change as it is shown: itself where there is nothing to replace.
            boolean nothing = true;
            for (char c : original) {
                if (TextUtils.indexOf(source, c) >= 0) {
                    nothing = false;
                    break;
                }
            }
            if (nothing) return source;
            // And where its spans cannot change either, the replacement done now, once.
            if (!(source instanceof Spannable)) {
                if (source instanceof Spanned) return new SpannedString(new SpannedReplacementCharSequence((Spanned) source, original, replacement));
                return new ReplacementCharSequence(source, original, replacement).toString();
            }
        }
        if (source instanceof Spanned) return new SpannedReplacementCharSequence((Spanned) source, original, replacement);
        return new ReplacementCharSequence(source, original, replacement);
    }

    public void onFocusChanged(View view, CharSequence sourceText, boolean focused, int direction, Rect previouslyFocusedRect) {}

    /** The source's characters as they are now, those of `original` replaced. */
    private static class ReplacementCharSequence implements CharSequence, GetChars {
        private final CharSequence mSource;
        private final char[] mOriginal, mReplacement;

        ReplacementCharSequence(CharSequence source, char[] original, char[] replacement) {
            mSource = source;
            mOriginal = original;
            mReplacement = replacement;
        }

        public int length() {
            return mSource.length();
        }

        public char charAt(int i) {
            char c = mSource.charAt(i);
            for (int j = 0; j < mOriginal.length; j++) if (c == mOriginal[j]) c = mReplacement[j];
            return c;
        }

        public CharSequence subSequence(int start, int end) {
            char[] c = new char[end - start];
            getChars(start, end, c, 0);
            return new String(c);
        }

        @Override
        public String toString() {
            char[] c = new char[length()];
            getChars(0, length(), c, 0);
            return new String(c);
        }

        public void getChars(int start, int end, char[] dest, int off) {
            TextUtils.getChars(mSource, start, end, dest, off);
            int stop = end - start + off;
            for (int i = off; i < stop; i++) {
                char c = dest[i];
                for (int j = 0; j < mOriginal.length; j++) if (c == mOriginal[j]) dest[i] = mReplacement[j];
            }
        }
    }

    /** The same over a Spanned source, whose spans it answers as its own (they are at the same places). */
    private static class SpannedReplacementCharSequence extends ReplacementCharSequence implements Spanned {
        private final Spanned mSpanned;

        SpannedReplacementCharSequence(Spanned source, char[] original, char[] replacement) {
            super(source, original, replacement);
            mSpanned = source;
        }

        @Override
        public CharSequence subSequence(int start, int end) {
            return new SpannedString(this).subSequence(start, end);
        }

        public <T> T[] getSpans(int start, int end, Class<T> type) {
            return mSpanned.getSpans(start, end, type);
        }

        public int getSpanStart(Object tag) {
            return mSpanned.getSpanStart(tag);
        }

        public int getSpanEnd(Object tag) {
            return mSpanned.getSpanEnd(tag);
        }

        public int getSpanFlags(Object tag) {
            return mSpanned.getSpanFlags(tag);
        }

        public int nextSpanTransition(int start, int end, Class type) {
            return mSpanned.nextSpanTransition(start, end, type);
        }
    }
}
