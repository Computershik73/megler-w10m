// Overlay of core/framework/gen-src/android/graphics/drawable/AdaptiveIconDrawable.java (tools/build-framework.sh).
// The generated class had the platform's signatures and nothing behind them: it drew nothing and its mask was null.
// Launchers make one to learn the icon's shape (Launcher3's GraphicsUtils.getShapePath: new Path(getIconMask())), and
// Nova Launcher 8.9.4 ended at its start on that null. This one is Android's, as far as an icon needs it:
//  - two layers, a background and a foreground, and a monochrome one kept for those who ask for it;
//  - each layer is half as large again as the icon, centred on it (the 25% of extra inset on every side), so the icon
//    shows the middle two thirds of it, as on Android;
//  - the mask is the platform's config_icon_mask (a path on 100 x 100), scaled to the bounds; the icon is drawn clipped by
//    it.
// adaptive-icon resources are made into one by megler.ResInflate.
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Matrix;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Region;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class AdaptiveIconDrawable extends Drawable implements Drawable.Callback {
    public static final float MASK_SIZE = 100.0f;
    private static final float EXTRA_INSET_PERCENTAGE = 1 / 4f;
    private static final float DEFAULT_VIEW_PORT_SCALE = 1f / (1 + 2 * EXTRA_INSET_PERCENTAGE);
    /** Android's own default (config.xml): a circle. */
    private static final String CIRCLE = "M50 0C77.6 0 100 22.4 100 50C100 77.6 77.6 100 50 100C22.4 100 0 77.6 0 50C0 22.4 22.4 0 50 0Z";
    private static Path sMask;

    private Drawable mBackground, mForeground, mMonochrome;
    private final Path mMask = new Path();
    private final Rect mLayerBounds = new Rect();
    private int mAlpha = 255, mOpacity = PixelFormat.TRANSLUCENT;

    public AdaptiveIconDrawable(Drawable background, Drawable foreground) { this(background, foreground, null); }
    public AdaptiveIconDrawable(Drawable background, Drawable foreground, Drawable monochrome) {
        mBackground = background;
        mForeground = foreground;
        mMonochrome = monochrome;
        for (Drawable d : new Drawable[] { mBackground, mForeground, mMonochrome })
            if (d != null) d.setCallback(this);
        mMask.set(mask());
    }
    AdaptiveIconDrawable() { this((Drawable) null, (Drawable) null); }

    /**
     * The platform's mask on 100 x 100: config_icon_mask (Android 9's emulator image has a rounded square), or a circle
     * when it cannot be read. By its name: megler.IR's numbers are another release's, and its number there is another
     * string here.
     */
    private static synchronized Path mask() {
        if (sMask == null) {
            Path p = null;
            try {
                android.content.res.Resources sys = android.content.res.Resources.getSystem();
                int id = sys.getIdentifier("config_icon_mask", "string", "android");
                String d = id != 0 ? sys.getString(id) : null;
                if (d != null && !d.trim().isEmpty()) p = megler.PathData.parse(d);
            } catch (RuntimeException e) {
                android.util.Log.w("Megler", "the platform's icon mask could not be read; a circle instead: " + e);
            }
            sMask = p != null && !p.isEmpty() ? p : megler.PathData.parse(CIRCLE);
        }
        return sMask;
    }

    public static float getExtraInsetFraction() { return EXTRA_INSET_PERCENTAGE; }
    public static float getExtraInsetPercentage() { return EXTRA_INSET_PERCENTAGE; }

    public Drawable getBackground() { return mBackground; }
    public Drawable getForeground() { return mForeground; }
    public Drawable getMonochrome() { return mMonochrome; }
    /** The mask in the bounds' coordinates. */
    public Path getIconMask() { return mMask; }
    /** The middle that is never cut off: 66 of the 72 units of the icon (Android's SAFEZONE_SCALE), as a rectangle. */
    public Region getSafeZone() {
        Rect b = getBounds();
        int dx = Math.round(b.width() * (1 - 66f / 72f) / 2), dy = Math.round(b.height() * (1 - 66f / 72f) / 2);
        return new Region(b.left + dx, b.top + dy, b.right - dx, b.bottom - dy);
    }
    public int getSourceDrawableResId() { return 0; }
    public boolean isProjected() { return false; }

    @Override protected void onBoundsChange(Rect b) {
        // The layers: half as large again as the icon, centred on it.
        int cx = b.centerX(), cy = b.centerY();
        int hw = (int) (b.width() / (DEFAULT_VIEW_PORT_SCALE * 2)), hh = (int) (b.height() / (DEFAULT_VIEW_PORT_SCALE * 2));
        mLayerBounds.set(cx - hw, cy - hh, cx + hw, cy + hh);
        for (Drawable d : new Drawable[] { mBackground, mForeground, mMonochrome })
            if (d != null) d.setBounds(mLayerBounds);
        // The mask: scaled from 100 x 100 to the bounds.
        Matrix m = new Matrix();
        m.setScale(b.width() / MASK_SIZE, b.height() / MASK_SIZE);
        m.postTranslate(b.left, b.top);
        mMask.set(mask());
        mMask.transform(m);
    }

    @Override public void draw(Canvas c) {
        Rect b = getBounds();
        if (b.isEmpty()) return;
        int saved = mAlpha < 255 ? c.saveLayerAlpha(b.left, b.top, b.right, b.bottom, mAlpha) : c.save();
        c.clipPath(mMask);
        if (mBackground != null) mBackground.draw(c);
        if (mForeground != null) mForeground.draw(c);
        c.restoreToCount(saved);
    }

    @Override public int getIntrinsicWidth() {
        int w = Math.max(mBackground != null ? mBackground.getIntrinsicWidth() : -1, mForeground != null ? mForeground.getIntrinsicWidth() : -1);
        return w <= 0 ? w : (int) (w * DEFAULT_VIEW_PORT_SCALE);
    }
    @Override public int getIntrinsicHeight() {
        int h = Math.max(mBackground != null ? mBackground.getIntrinsicHeight() : -1, mForeground != null ? mForeground.getIntrinsicHeight() : -1);
        return h <= 0 ? h : (int) (h * DEFAULT_VIEW_PORT_SCALE);
    }

    @Override public void setAlpha(int a) { if (mAlpha != a) { mAlpha = a; invalidateSelf(); } }
    @Override public int getAlpha() { return mAlpha; }
    @Override public void setColorFilter(ColorFilter f) {
        super.setColorFilter(f);
        if (mBackground != null) mBackground.setColorFilter(f);
        if (mForeground != null) mForeground.setColorFilter(f);
    }
    @Override public void setTintList(android.content.res.ColorStateList t) {
        if (mBackground != null) mBackground.setTintList(t);
        if (mForeground != null) mForeground.setTintList(t);
    }
    @Override public int getOpacity() { return mOpacity; }
    public void setOpacity(int o) { mOpacity = o; }
    @Override public boolean isStateful() {
        return (mBackground != null && mBackground.isStateful()) || (mForeground != null && mForeground.isStateful());
    }
    @Override protected boolean onStateChange(int[] state) {
        boolean changed = false;
        if (mBackground != null) changed |= mBackground.setState(state);
        if (mForeground != null) changed |= mForeground.setState(state);
        return changed;
    }
    @Override public void getOutline(android.graphics.Outline o) { o.setConvexPath(mMask); }
    @Override public Drawable mutate() {
        if (mBackground != null) mBackground.mutate();
        if (mForeground != null) mForeground.mutate();
        return this;
    }
    public void clearMutated() {}

    @Override public void invalidateDrawable(Drawable who) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }
}
