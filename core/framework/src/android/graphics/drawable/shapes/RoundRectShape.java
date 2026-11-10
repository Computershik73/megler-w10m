package android.graphics.drawable.shapes;
import android.graphics.*;
/*
 * Megler: Android's. The rectangle within (an inset) is a hole only where it is smaller than the shape both ways; an inset of
 * nothing, as Messenger's rounded backgrounds give (RoundRectShape(radii, new RectF(), null), its search field's among them),
 * is no hole, and the shape is filled whole. The hole was cut whatever its size, even-odd: an inset of nothing cut the
 * rounded rectangle out of its own bounds, and only the four corners outside its curves were filled -- the search field a
 * black pill with grey corners where Android draws a grey pill. The path is made at each size, and the outline is the
 * rounded rectangle (a path where the radii differ) for a shape without an inset and none for one with an inset, as
 * Android's.
 */
public class RoundRectShape extends RectShape {
    private float[] mOuterRadii;
    private RectF mInset;
    private float[] mInnerRadii;
    private RectF mInnerRect;
    private Path mPath;

    public RoundRectShape(float[] outerRadii, RectF inset, float[] innerRadii) {
        if (outerRadii != null && outerRadii.length < 8) throw new ArrayIndexOutOfBoundsException("outer radii must have >= 8 values");
        if (innerRadii != null && innerRadii.length < 8) throw new ArrayIndexOutOfBoundsException("inner radii must have >= 8 values");
        mOuterRadii = outerRadii;
        mInset = inset;
        mInnerRadii = innerRadii;
        if (inset != null) mInnerRect = new RectF();
        mPath = new Path();
    }

    @Override public void draw(Canvas c, Paint p) { c.drawPath(mPath, p); }

    @Override public void getOutline(Outline outline) {
        if (mInnerRect != null) return; // an inset, which may be a hole: no outline (Android's)
        float radius = 0;
        if (mOuterRadii != null) {
            radius = mOuterRadii[0];
            for (int i = 1; i < 8; i++) {
                if (mOuterRadii[i] != radius) {
                    outline.setPath(mPath);
                    return;
                }
            }
        }
        final RectF r = rect();
        outline.setRoundRect((int) Math.ceil(r.left), (int) Math.ceil(r.top), (int) Math.floor(r.right), (int) Math.floor(r.bottom), radius);
    }

    @Override protected void onResize(float w, float h) {
        super.onResize(w, h);
        final RectF r = rect();
        mPath.reset();
        mPath.setFillType(Path.FillType.WINDING);
        if (mOuterRadii != null) mPath.addRoundRect(r, mOuterRadii, Path.Direction.CW);
        else mPath.addRect(r, Path.Direction.CW);
        if (mInnerRect != null) {
            mInnerRect.set(r.left + mInset.left, r.top + mInset.top, r.right - mInset.right, r.bottom - mInset.bottom);
            if (mInnerRect.width() < r.width() && mInnerRect.height() < r.height()) {
                if (mInnerRadii != null) mPath.addRoundRect(mInnerRect, mInnerRadii, Path.Direction.CCW);
                else mPath.addRect(mInnerRect, Path.Direction.CCW);
                // (Android's hole is the winding of the inner path's other direction; even-odd makes the same one.)
                mPath.setFillType(Path.FillType.EVEN_ODD);
            }
        }
    }

    @Override public RoundRectShape clone() throws CloneNotSupportedException {
        final RoundRectShape shape = (RoundRectShape) super.clone();
        shape.mOuterRadii = mOuterRadii != null ? mOuterRadii.clone() : null;
        shape.mInnerRadii = mInnerRadii != null ? mInnerRadii.clone() : null;
        shape.mInset = mInset != null ? new RectF(mInset) : null;
        shape.mInnerRect = mInnerRect != null ? new RectF(mInnerRect) : null;
        shape.mPath = new Path(mPath);
        return shape;
    }
}
