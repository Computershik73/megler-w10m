package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;

/** Fills its bounds with the colour a ColorStateList gives for the current state. */
public class ColorStateListDrawable extends Drawable implements Drawable.Callback {
    private ColorStateList mColors;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int mAlpha = -1;
    public ColorStateListDrawable() { this(ColorStateList.valueOf(0xFF000000)); }
    public ColorStateListDrawable(ColorStateList c) { mColors = c; }
    public ColorStateList getColorStateList() { return mColors; }
    public void setColorStateList(ColorStateList c) { mColors = c; invalidateSelf(); }
    private int color() {
        int c = mColors == null ? 0 : mColors.getColorForState(getState(), mColors.getDefaultColor());
        if (mAlpha >= 0) c = (c & 0x00FFFFFF) | ((((c >>> 24) * mAlpha) / 255) << 24);
        return c;
    }
    @Override public void draw(Canvas c) { mPaint.setColor(color()); c.drawRect(getBounds(), mPaint); }
    @Override public void setAlpha(int a) { mAlpha = a; invalidateSelf(); }
    @Override public int getAlpha() { return mAlpha >= 0 ? mAlpha : (color() >>> 24); }
    public void clearAlpha() { mAlpha = -1; invalidateSelf(); }
    @Override public void setColorFilter(ColorFilter f) { mPaint.setColorFilter(f); invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return mPaint.getColorFilter(); }
    @Override public int getOpacity() { int a = color() >>> 24; return a == 255 ? PixelFormat.OPAQUE : a == 0 ? PixelFormat.TRANSPARENT : PixelFormat.TRANSLUCENT; }
    @Override public boolean isStateful() { return mColors != null && mColors.isStateful(); }
    @Override protected boolean onStateChange(int[] state) { invalidateSelf(); return isStateful(); }
    public boolean hasFocusStateSpecified() { return false; }
    @Override public Drawable mutate() { return this; }
    /* Megler: the constant state, which Android's has: a copy is a drawable of these colours, at this one's alpha and colour
       filter as they are when the copy is asked for (DrawableWrapper says why a drawable Android gives a state has one). */
    private ConstantState mMeglerState;
    @Override public ConstantState getConstantState() {
        if (mMeglerState == null) mMeglerState = new ConstantState() {
            @Override public Drawable newDrawable() {
                ColorStateListDrawable c = new ColorStateListDrawable(mColors);
                c.mAlpha = mAlpha;
                c.mPaint.setColorFilter(mPaint.getColorFilter());
                return c;
            }
        };
        return mMeglerState;
    }
    public void invalidateDrawable(Drawable d) { invalidateSelf(); }
    public void scheduleDrawable(Drawable d, Runnable r, long when) { scheduleSelf(r, when); }
    public void unscheduleDrawable(Drawable d, Runnable r) { unscheduleSelf(r); }
}
