// Overlay of core/framework/src/android/graphics/drawable/InsetDrawable.java (tools/build-framework.sh).
// It differs in its insets, each of which is a fraction of the bounds and pixels, as Android's (8.0 and later, its
// InsetDrawable.InsetValue): the constructors that take fractions keep them (InsetDrawable(Drawable, float) made one with no
// inset at all), and megler.ResInflate gives an <inset> whose android:inset or android:insetLeft ... is a fraction ("10%",
// TypedValue.TYPE_FRACTION) as one (meglerInsets). The drawable's bounds, its intrinsic size and its padding are worked out
// from them as Android's are.
//
// Why. WhatsApp's adaptive icon (mipmap/icon, Settings > Chats > Theme and its Appearance screen) has an inset of 0.01% round
// its foreground. The inflater read it with getDimensionPixelOffset, which throws for a fraction as Android's does, and the
// app ended: "Can't convert value at index 6 to dimension: type=0x6" in "File res/ksi.xml from drawable resource ID
// #0x7f130002" (0.1.0.671). Android's InsetDrawable.inflate looks at the value's type first.
package android.graphics.drawable;

import android.graphics.*;

public class InsetDrawable extends DrawableWrapper {
    /** One inset: a fraction of the bounds' width or height and pixels besides, as Android's InsetValue. */
    private static final class InsetValue {
        final float fraction;
        final int pixels;
        InsetValue(float fraction, int pixels) { this.fraction = fraction; this.pixels = pixels; }
        /** The inset in pixels for bounds of that size, as Android's InsetValue.getDimension. */
        int of(int size) { return (int) (size * fraction) + pixels; }
    }
    private final InsetValue l, t, r, b;
    private final Rect mTmp = new Rect();

    public InsetDrawable(Drawable d, int inset) { this(d, inset, inset, inset, inset); }
    public InsetDrawable(Drawable d, float inset) { this(d, inset, inset, inset, inset); }
    public InsetDrawable(Drawable d, int l, int t, int r, int b) { this(d, new InsetValue(0f, l), new InsetValue(0f, t), new InsetValue(0f, r), new InsetValue(0f, b)); }
    public InsetDrawable(Drawable d, float l, float t, float r, float b) { this(d, new InsetValue(l, 0), new InsetValue(t, 0), new InsetValue(r, 0), new InsetValue(b, 0)); }
    private InsetDrawable(Drawable d, InsetValue l, InsetValue t, InsetValue r, InsetValue b) {
        super(null);
        this.l = l; this.t = t; this.r = r; this.b = b;
        setDrawable(d);
    }

    /**
     * Megler: an inset drawable whose each inset is a fraction of the bounds and pixels besides, as an <inset> gives them
     * (megler.ResInflate): left, top, right and bottom in that order.
     */
    public static InsetDrawable meglerInsets(Drawable d, float[] fractions, int[] pixels) {
        return new InsetDrawable(d, new InsetValue(fractions[0], pixels[0]), new InsetValue(fractions[1], pixels[1]),
                                 new InsetValue(fractions[2], pixels[2]), new InsetValue(fractions[3], pixels[3]));
    }

    /** The insets for these bounds, in pixels. */
    private void insets(Rect bounds, Rect out) {
        int w = bounds.width(), h = bounds.height();
        out.set(l.of(w), t.of(h), r.of(w), b.of(h));
    }

    @Override protected void onBoundsChange(Rect bo) {
        Drawable d = getDrawable();
        if (d == null) return;
        insets(bo, mTmp);
        d.setBounds(bo.left + mTmp.left, bo.top + mTmp.top, bo.right - mTmp.right, bo.bottom - mTmp.bottom);
    }
    /* As Android's: the drawable's own size made the part of the whole that the fractions leave, and the pixels added. */
    @Override public int getIntrinsicWidth() {
        int w = super.getIntrinsicWidth();
        float f = l.fraction + r.fraction;
        if (w < 0 || f >= 1) return -1;
        return (int) (w / (1 - f)) + l.pixels + r.pixels;
    }
    @Override public int getIntrinsicHeight() {
        int h = super.getIntrinsicHeight();
        float f = t.fraction + b.fraction;
        if (h < 0 || f >= 1) return -1;
        return (int) (h / (1 - f)) + t.pixels + b.pixels;
    }
    @Override public boolean getPadding(Rect p) {
        boolean has = super.getPadding(p);
        insets(getBounds(), mTmp);
        p.left += mTmp.left; p.top += mTmp.top; p.right += mTmp.right; p.bottom += mTmp.bottom;
        return has || (mTmp.left | mTmp.top | mTmp.right | mTmp.bottom) != 0;
    }
}
