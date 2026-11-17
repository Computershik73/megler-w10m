package android.graphics.drawable;

import android.content.res.Resources;
import android.graphics.*;

public class BitmapDrawable extends Drawable {
    private Bitmap mBitmap;
    private final Paint mPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG);
    private int mGravity = android.view.Gravity.FILL, mTargetDensity = android.util.DisplayMetrics.DENSITY_DEVICE_STABLE;
    private Shader.TileMode mTileX, mTileY;
    private BitmapShader mShader;
    private final Rect mDst = new Rect();

    public BitmapDrawable() {}
    public BitmapDrawable(Bitmap b) { this(null, b); }
    public BitmapDrawable(Resources r, Bitmap b) { mBitmap = b; if (r != null) mTargetDensity = r.getDisplayMetrics().densityDpi; }
    public BitmapDrawable(String path) { this(null, BitmapFactory.decodeFile(path)); }
    public BitmapDrawable(Resources r, String path) { this(r, BitmapFactory.decodeFile(path)); }
    public BitmapDrawable(java.io.InputStream in) { this(null, BitmapFactory.decodeStream(in)); }
    public BitmapDrawable(Resources r, java.io.InputStream in) { this(r, BitmapFactory.decodeStream(in)); }
    public final Paint getPaint() { return mPaint; }
    public final Bitmap getBitmap() { return mBitmap; }
    public void setBitmap(Bitmap b) { mBitmap = b; mShader = null; invalidateSelf(); }
    public void setTargetDensity(int d) { mTargetDensity = d == 0 ? 160 : d; }
    public void setTargetDensity(Canvas c) { setTargetDensity(c.getDensity()); }
    public void setTargetDensity(android.util.DisplayMetrics m) { setTargetDensity(m.densityDpi); }
    public int getGravity() { return mGravity; }
    public void setGravity(int g) { mGravity = g; invalidateSelf(); }
    public void setMipMap(boolean m) {}
    public boolean hasMipMap() { return false; }
    public void setAntiAlias(boolean aa) { mPaint.setAntiAlias(aa); }
    public boolean hasAntiAlias() { return mPaint.isAntiAlias(); }
    @Override public void setFilterBitmap(boolean f) { mPaint.setFilterBitmap(f); }
    @Override public boolean isFilterBitmap() { return mPaint.isFilterBitmap(); }
    public Shader.TileMode getTileModeX() { return mTileX; }
    public Shader.TileMode getTileModeY() { return mTileY; }
    public void setTileModeX(Shader.TileMode m) { setTileModeXY(m, mTileY); }
    public void setTileModeY(Shader.TileMode m) { setTileModeXY(mTileX, m); }
    public void setTileModeXY(Shader.TileMode x, Shader.TileMode y) { mTileX = x; mTileY = y; mShader = null; invalidateSelf(); }
    @Override public void setAlpha(int a) { mPaint.setAlpha(a); invalidateSelf(); }
    @Override public int getAlpha() { return mPaint.getAlpha(); }
    private int scaled(int v) { if (mBitmap == null) return -1; int d = mBitmap.getDensity(); return d == 0 || d == mTargetDensity ? v : (v * mTargetDensity + d / 2) / d; }
    @Override public int getIntrinsicWidth() { return mBitmap == null ? -1 : scaled(mBitmap.getWidth()); }
    @Override public int getIntrinsicHeight() { return mBitmap == null ? -1 : scaled(mBitmap.getHeight()); }
    @Override public int getOpacity() { return mBitmap == null || mBitmap.hasAlpha() || mPaint.getAlpha() < 255 ? PixelFormat.TRANSLUCENT : PixelFormat.OPAQUE; }
    @Override public void draw(Canvas c) {
        if (mBitmap == null || mBitmap.isRecycled()) return;
        mPaint.setColorFilter(meglerTintFilter());
        Rect b = getBounds();
        if (mTileX != null || mTileY != null) {
            if (mShader == null) mShader = new BitmapShader(mBitmap, mTileX == null ? Shader.TileMode.CLAMP : mTileX, mTileY == null ? Shader.TileMode.CLAMP : mTileY);
            mPaint.setShader(mShader);
            c.drawRect(b, mPaint);
            mPaint.setShader(null);
            return;
        }
        android.view.Gravity.apply(mGravity, getIntrinsicWidth(), getIntrinsicHeight(), b, mDst);
        c.drawBitmap(mBitmap, null, mDst, mPaint);
    }
    /*
     * The constant state: the bitmap with its gravity, tiling, density, alpha, the paint's flags and the tint, as Android's
     * BitmapState holds them, as they are when the state is asked for; a copy has them all (and another Resources' density
     * where one is given). It had the bitmap alone: WhatsApp's dim slider gives Material's Slider a thumb whose icon is a
     * tinted <bitmap>, which the Slider copies through the state, and the copy was drawn untinted. (Taken when asked, not
     * when a copy is made: Resources keeps the state of an image it read first, and mutate() here is the drawable itself,
     * so what an app sets on that one after mutate() must not reach the copies Resources gives later.)
     */
    @Override public ConstantState getConstantState() { return new BitmapState(this); }
    static final class BitmapState extends ConstantState {
        private final Bitmap mBitmap;
        private final int mGravity, mDensity, mAlpha;
        private final Shader.TileMode mTileX, mTileY;
        private final boolean mAntiAlias, mFilter, mDither;
        private final android.content.res.ColorStateList mTint;
        private final PorterDuff.Mode mTintMode;
        BitmapState(BitmapDrawable d) {
            mBitmap = d.mBitmap; mGravity = d.mGravity; mDensity = d.mTargetDensity; mAlpha = d.mPaint.getAlpha();
            mTileX = d.mTileX; mTileY = d.mTileY;
            mAntiAlias = d.mPaint.isAntiAlias(); mFilter = d.mPaint.isFilterBitmap(); mDither = d.mPaint.isDither();
            mTint = d.mTintList; mTintMode = d.mTintMode;
        }
        @Override public Drawable newDrawable() { return newDrawable(null); }
        @Override public Drawable newDrawable(Resources r) {
            BitmapDrawable c = new BitmapDrawable(r, mBitmap);
            if (r == null) c.mTargetDensity = mDensity;
            c.mGravity = mGravity;
            c.mTileX = mTileX;
            c.mTileY = mTileY;
            c.mPaint.setAntiAlias(mAntiAlias);
            c.mPaint.setFilterBitmap(mFilter);
            c.mPaint.setDither(mDither);
            c.mPaint.setAlpha(mAlpha);
            c.mTintList = mTint;
            c.mTintMode = mTintMode;
            return c;
        }
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public BitmapDrawable(android.content.res.Resources p0) { this(); }
    public void clearMutated() {}
    public void setXfermode(android.graphics.Xfermode p0) {}
    // ---- end of generated members
}
