package android.graphics.drawable.shapes;
import android.graphics.*;
public class OvalShape extends RectShape {
    @Override public void draw(Canvas c, Paint p) { c.drawOval(rect(), p); }
    /* Megler: Android's outline, the oval (it was the rectangle's). */
    @Override public void getOutline(Outline outline) {
        final RectF r = rect();
        outline.setOval((int) Math.ceil(r.left), (int) Math.ceil(r.top), (int) Math.floor(r.right), (int) Math.floor(r.bottom));
    }
    @Override public OvalShape clone() throws CloneNotSupportedException { return (OvalShape) super.clone(); }
}
