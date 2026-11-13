// Overlay of core/framework/gen-src/android/text/PrecomputedText.java (tools/build-framework.sh).
// The generated one was signatures alone: create() gave a text of no characters whose getSpans was null, and its
// parameters kept nothing of what a Builder was told. androidx's PrecomputedTextCompat hands the platform's object on
// from Android 10, so an app that made a SpannableString of one ended in a NullPointerException (Ozon's chat).
// This one is the text: a copy of the characters and the spans, cut into paragraphs at line feeds, with the parameters it
// was made for. Nothing is measured ahead -- a layout measures this text as it does any other -- so a paragraph has no
// MeasuredParagraph, and getWidth, getBounds and getCharWidthAt measure with the parameters' paint when they are asked
// (the spans that change a run's metrics are not counted there).
package android.text;

import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.text.LineBreakConfig;
import android.text.style.MetricAffectingSpan;
import java.util.ArrayList;

public class PrecomputedText implements Spannable {
    private static final char LINE_FEED = '\n';

    /** The parameters a text is measured with: the paint, the direction and how its lines are broken. */
    public static final class Params {
        public static final int UNUSABLE = 0, NEED_RECOMPUTE = 1, USABLE = 2;
        private static LineBreakConfig sNoConfig;
        private final TextPaint mPaint;
        private final TextDirectionHeuristic mTextDir;
        private final int mBreakStrategy, mHyphenationFrequency;
        private final LineBreakConfig mLineBreakConfig;

        /** The configuration of one who gave none: no style of either kind. */
        static synchronized LineBreakConfig noConfig() {
            if (LineBreakConfig.NONE != null) return LineBreakConfig.NONE;
            if (sNoConfig == null) sNoConfig = new LineBreakConfig.Builder().build();
            return sNoConfig;
        }
        private static boolean sameConfig(LineBreakConfig a, LineBreakConfig b) {
            if (a == null) a = noConfig();
            if (b == null) b = noConfig();
            return a == b || (a.getLineBreakStyle() == b.getLineBreakStyle() && a.getLineBreakWordStyle() == b.getLineBreakWordStyle());
        }

        public static class Builder {
            private final TextPaint mPaint;
            private TextDirectionHeuristic mTextDir = TextDirectionHeuristics.FIRSTSTRONG_LTR;
            private int mBreakStrategy = Layout.BREAK_STRATEGY_HIGH_QUALITY, mHyphenationFrequency = Layout.HYPHENATION_FREQUENCY_NORMAL;
            private LineBreakConfig mLineBreakConfig = noConfig();
            public Builder(TextPaint paint) { mPaint = paint; }
            public Builder(Params params) {
                mPaint = params.mPaint; mTextDir = params.mTextDir; mBreakStrategy = params.mBreakStrategy;
                mHyphenationFrequency = params.mHyphenationFrequency; mLineBreakConfig = params.mLineBreakConfig;
            }
            public Builder setBreakStrategy(int strategy) { mBreakStrategy = strategy; return this; }
            public Builder setHyphenationFrequency(int frequency) { mHyphenationFrequency = frequency; return this; }
            public Builder setTextDirection(TextDirectionHeuristic textDir) { mTextDir = textDir; return this; }
            public Builder setLineBreakConfig(LineBreakConfig lineBreakConfig) { mLineBreakConfig = lineBreakConfig == null ? noConfig() : lineBreakConfig; return this; }
            public Params build() { return new Params(mPaint, mLineBreakConfig, mTextDir, mBreakStrategy, mHyphenationFrequency); }
        }

        public Params(TextPaint paint, LineBreakConfig lineBreakConfig, TextDirectionHeuristic textDir, int strategy, int frequency) {
            mPaint = paint; mTextDir = textDir; mBreakStrategy = strategy; mHyphenationFrequency = frequency;
            mLineBreakConfig = lineBreakConfig == null ? noConfig() : lineBreakConfig;
        }
        public TextPaint getTextPaint() { return mPaint; }
        public TextDirectionHeuristic getTextDirection() { return mTextDir; }
        public int getBreakStrategy() { return mBreakStrategy; }
        public int getHyphenationFrequency() { return mHyphenationFrequency; }
        public LineBreakConfig getLineBreakConfig() { return mLineBreakConfig; }

        /** Whether a text measured with these parameters serves one who has the given: USABLE, NEED_RECOMPUTE where the
         *  direction alone differs, or UNUSABLE. */
        public int checkResultUsable(TextPaint paint, TextDirectionHeuristic textDir, int strategy, int frequency, LineBreakConfig lbConfig) {
            if (mBreakStrategy == strategy && mHyphenationFrequency == frequency && sameConfig(mLineBreakConfig, lbConfig)
                    && paint != null && (mPaint == paint || mPaint.equalsForTextMeasurement(paint)))
                return mTextDir == textDir ? USABLE : NEED_RECOMPUTE;
            return UNUSABLE;
        }
        @Override public boolean equals(Object o) {
            if (o == this) return true;
            if (!(o instanceof Params)) return false;
            Params p = (Params) o;
            return checkResultUsable(p.mPaint, p.mTextDir, p.mBreakStrategy, p.mHyphenationFrequency, p.mLineBreakConfig) == USABLE;
        }
        @Override public int hashCode() {
            return java.util.Objects.hash(mPaint.getTextSize(), mPaint.getTextScaleX(), mPaint.getTextSkewX(), mPaint.getLetterSpacing(), mPaint.getTypeface(),
                mTextDir, mBreakStrategy, mHyphenationFrequency, mLineBreakConfig.getLineBreakStyle(), mLineBreakConfig.getLineBreakWordStyle());
        }
        @Override public String toString() {
            return "{textSize=" + mPaint.getTextSize() + ", textScaleX=" + mPaint.getTextScaleX() + ", textSkewX=" + mPaint.getTextSkewX()
                + ", letterSpacing=" + mPaint.getLetterSpacing() + ", textLocale=" + mPaint.getTextLocales() + ", typeface=" + mPaint.getTypeface()
                + ", variationSettings=" + mPaint.getFontVariationSettings() + ", elegantTextHeight=" + mPaint.isElegantTextHeight()
                + ", textDir=" + mTextDir + ", breakStrategy=" + mBreakStrategy + ", hyphenationFrequency=" + mHyphenationFrequency
                + ", lineBreakStyle=" + mLineBreakConfig.getLineBreakStyle() + ", lineBreakWordStyle=" + mLineBreakConfig.getLineBreakWordStyle() + "}";
        }
    }

    /** A paragraph of the text: where it ends, its line feed counted in. `measured` is null here: nothing is measured ahead. */
    public static class ParagraphInfo {
        public final int paragraphEnd;
        public final MeasuredParagraph measured;
        public ParagraphInfo(int paraEnd, MeasuredParagraph measured) { this.paragraphEnd = paraEnd; this.measured = measured; }
    }

    private final SpannableString mText;
    private final int mStart, mEnd;
    private final Params mParams;
    private final ParagraphInfo[] mParagraphInfo;

    /** The text with the parameters it will be laid out with. One that is such a text of the same parameters already is
     *  given back as it is. */
    public static PrecomputedText create(CharSequence text, Params params) {
        if (text == null) throw new NullPointerException("text");
        if (params == null) throw new NullPointerException("params");
        if (text instanceof PrecomputedText) {
            PrecomputedText made = (PrecomputedText) text;
            if (made.mParams.checkResultUsable(params.mPaint, params.mTextDir, params.mBreakStrategy, params.mHyphenationFrequency, params.mLineBreakConfig) == Params.USABLE)
                return made;
        }
        return new PrecomputedText(text, 0, text.length(), params, createMeasuredParagraphs(text, params, 0, text.length(), true, true));
    }

    /** The paragraphs of a text between two places: each ends after its line feed, the last at `end`. */
    public static ParagraphInfo[] createMeasuredParagraphs(CharSequence text, Params params, int start, int end, boolean computeHyphenation, boolean computeLayout) {
        if (text == null) throw new NullPointerException("text");
        if (params == null) throw new NullPointerException("params");
        ArrayList<ParagraphInfo> out = new ArrayList<>();
        int paraEnd;
        for (int paraStart = start; paraStart < end; paraStart = paraEnd) {
            paraEnd = TextUtils.indexOf(text, LINE_FEED, paraStart, end);
            paraEnd = paraEnd < 0 ? end : paraEnd + 1;
            out.add(new ParagraphInfo(paraEnd, null));
        }
        return out.toArray(new ParagraphInfo[0]);
    }

    private PrecomputedText(CharSequence text, int start, int end, Params params, ParagraphInfo[] paraInfo) {
        // Each span of the text, those a copy leaves out (NoCopySpan) too, as Android's keeps them.
        mText = new SpannableString(text.toString());
        if (text instanceof Spanned) {
            Spanned from = (Spanned) text;
            Object[] spans = from.getSpans(0, from.length(), Object.class);
            if (spans != null) for (Object o : spans) mText.setSpan(o, from.getSpanStart(o), from.getSpanEnd(o), from.getSpanFlags(o));
        }
        mStart = start;
        mEnd = end;
        mParams = params;
        mParagraphInfo = paraInfo;
    }

    public CharSequence getText() { return mText; }
    public int getStart() { return mStart; }
    public int getEnd() { return mEnd; }
    public Params getParams() { return mParams; }
    public int getParagraphCount() { return mParagraphInfo.length; }
    public int getParagraphStart(int paraIndex) {
        if (paraIndex < 0 || paraIndex >= mParagraphInfo.length) throw new IllegalArgumentException("paraIndex " + paraIndex + " of " + mParagraphInfo.length);
        return paraIndex == 0 ? mStart : mParagraphInfo[paraIndex - 1].paragraphEnd;
    }
    public int getParagraphEnd(int paraIndex) {
        if (paraIndex < 0 || paraIndex >= mParagraphInfo.length) throw new IllegalArgumentException("paraIndex " + paraIndex + " of " + mParagraphInfo.length);
        return mParagraphInfo[paraIndex].paragraphEnd;
    }
    public MeasuredParagraph getMeasuredParagraph(int paraIndex) { return mParagraphInfo[paraIndex].measured; }
    public ParagraphInfo[] getParagraphInfo() { return mParagraphInfo; }

    public int checkResultUsable(int start, int end, TextDirectionHeuristic textDir, TextPaint paint, int strategy, int frequency, LineBreakConfig lbConfig) {
        if (mStart != start || mEnd != end) return Params.UNUSABLE;
        return mParams.checkResultUsable(paint, textDir, strategy, frequency, lbConfig);
    }

    /** The paragraph a place of the text is in. */
    public int findParaIndex(int pos) {
        for (int i = 0; i < mParagraphInfo.length; i++) if (pos < mParagraphInfo[i].paragraphEnd) return i;
        throw new IndexOutOfBoundsException("pos must be less than " + (mParagraphInfo.length == 0 ? mEnd : mParagraphInfo[mParagraphInfo.length - 1].paragraphEnd) + ", gave " + pos);
    }

    /** That a range is one of the text and, where it is not empty, of one paragraph. */
    private void checkRange(int start, int end) {
        if (start < 0 || start > mText.length()) throw new IllegalArgumentException("invalid start offset " + start);
        if (end < 0 || end > mText.length()) throw new IllegalArgumentException("invalid end offset " + end);
        if (start > end) throw new IllegalArgumentException("start offset can not be larger than end offset");
        if (start == end) return;
        int para = findParaIndex(start), paraStart = getParagraphStart(para), paraEnd = getParagraphEnd(para);
        if (start < paraStart || paraEnd < end)
            throw new IllegalArgumentException("Cannot measured across the paragraph:para: (" + paraStart + ", " + paraEnd + "), request: (" + start + ", " + end + ")");
    }

    public float getWidth(int start, int end) {
        checkRange(start, end);
        return start == end ? 0 : mParams.getTextPaint().measureText(mText.toString(), start, end);
    }
    public void getBounds(int start, int end, Rect bounds) {
        if (bounds == null) throw new NullPointerException("bounds");
        checkRange(start, end);
        if (start == end) bounds.set(0, 0, 0, 0);
        else mParams.getTextPaint().getTextBounds(mText.toString(), start, end, bounds);
    }
    public void getFontMetricsInt(int start, int end, Paint.FontMetricsInt outMetrics) {
        if (outMetrics == null) throw new NullPointerException("outMetrics");
        checkRange(start, end);
        mParams.getTextPaint().getFontMetricsInt(outMetrics);
    }
    /** The width of the character at a place; of the two halves of a character written as a pair the first has it all. */
    public float getCharWidthAt(int offset) {
        if (offset < 0 || offset >= mText.length()) throw new IllegalArgumentException("invalid offset " + offset);
        char c = mText.charAt(offset);
        if (Character.isLowSurrogate(c) && offset > 0 && Character.isHighSurrogate(mText.charAt(offset - 1))) return 0;
        int end = Character.isHighSurrogate(c) && offset + 1 < mText.length() && Character.isLowSurrogate(mText.charAt(offset + 1)) ? offset + 2 : offset + 1;
        return mParams.getTextPaint().measureText(mText.toString(), offset, end);
    }
    /** The bytes this text holds: its characters. */
    public int getMemoryUsage() { return mText.length() * 2; }

    // Spannable: the copy's, but for a span that changes the metrics -- the text was measured without it, and Android
    // refuses it so.
    @Override public void setSpan(Object what, int start, int end, int flags) {
        if (what instanceof MetricAffectingSpan) throw new IllegalArgumentException("MetricAffectingSpan can not be set to PrecomputedText.");
        mText.setSpan(what, start, end, flags);
    }
    @Override public void removeSpan(Object what) {
        if (what instanceof MetricAffectingSpan) throw new IllegalArgumentException("MetricAffectingSpan can not be removed from PrecomputedText.");
        mText.removeSpan(what);
    }
    @Override public <T> T[] getSpans(int start, int end, Class<T> type) { return mText.getSpans(start, end, type); }
    @Override public int getSpanStart(Object tag) { return mText.getSpanStart(tag); }
    @Override public int getSpanEnd(Object tag) { return mText.getSpanEnd(tag); }
    @Override public int getSpanFlags(Object tag) { return mText.getSpanFlags(tag); }
    @Override public int nextSpanTransition(int start, int limit, Class type) { return mText.nextSpanTransition(start, limit, type); }
    @Override public int length() { return mText.length(); }
    @Override public char charAt(int index) { return mText.charAt(index); }
    @Override public CharSequence subSequence(int start, int end) { return PrecomputedText.create(mText.subSequence(start, end), mParams); }
    @Override public String toString() { return mText.toString(); }
}
