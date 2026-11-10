package android.graphics.drawable.shapes;
import android.graphics.*;
public class RectShape extends Shape {
    private RectF mRect = new RectF();
    @Override public void draw(Canvas c, Paint p) { c.drawRect(mRect, p); }
    @Override protected void onResize(float w, float h) { mRect.set(0, 0, w, h); }
    protected final RectF rect() { return mRect; }
    /* Megler: Android's outline of the rectangle, and a clone with a rectangle of its own (they were Shape's: none, and the one shared). */
    @Override public void getOutline(Outline outline) {
        final RectF r = rect();
        outline.setRect((int) Math.ceil(r.left), (int) Math.ceil(r.top), (int) Math.floor(r.right), (int) Math.floor(r.bottom));
    }
    @Override public RectShape clone() throws CloneNotSupportedException {
        final RectShape shape = (RectShape) super.clone();
        shape.mRect = new RectF(mRect);
        return shape;
    }
}
