package meglet.test.hello;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.GradientDrawable;
import android.text.Layout;
import android.text.SpannableString;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.style.RelativeSizeSpan;
import android.util.AttributeSet;
import android.view.View;

/**
 * A view that draws for itself: a blue field, a red rectangle at a known place in it, a green one whose width follows the count, a shape
 * that has rounded corners and no colour but its tint, and two letters twice: made twice the paint's size by a span, and by the paint.
 */
public class Swatch extends View {
    private final Paint paint = new Paint();
    private final GradientDrawable pill = new GradientDrawable();
    private final TextPaint small = new TextPaint(), large = new TextPaint();
    private final StaticLayout spanned, plain;
    int count;

    public Swatch(Context context, AttributeSet attrs) {
        super(context, attrs);
        /* Nothing says what colour this shape is; Android fills a tinted shape with its tint, and a tab's indicator is made this way. */
        pill.setCornerRadius(8);
        pill.setTint(0xFFC06020);
        /* A span that doubles the size must give what a paint of double the size gives, in the layout's measure and in what is drawn. */
        small.setColor(0xFF800080);
        small.setTextSize(12);
        large.setColor(0xFF800080);
        large.setTextSize(24);
        SpannableString twice = new SpannableString("HH");
        twice.setSpan(new RelativeSizeSpan(2f), 0, 2, 0);
        spanned = new StaticLayout(twice, small, 130, Layout.Alignment.ALIGN_NORMAL, 1f, 0f, false);
        plain = new StaticLayout("HH", large, 130, Layout.Alignment.ALIGN_NORMAL, 1f, 0f, false);
    }

    void setCount(int n) {
        count = n;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawColor(getResources().getColor(R.color.swatch));
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFFFF0000);
        canvas.drawRect(10, 10, 50, 30, paint);
        paint.setColor(0xFF00C000);
        canvas.save();
        canvas.translate(60, 10);
        canvas.drawRect(0, 0, 20 * (count + 1), 20, paint);
        canvas.restore();
        pill.setBounds(10, 50, 70, 80);
        pill.draw(canvas);
        canvas.save();
        canvas.translate(200, 40);
        spanned.draw(canvas);
        canvas.translate(140, 0);
        plain.draw(canvas);
        canvas.restore();
        MainActivity.draws++;
    }
}
