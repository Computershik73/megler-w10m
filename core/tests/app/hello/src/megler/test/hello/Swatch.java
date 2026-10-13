package megler.test.hello;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

/** A view that draws for itself: a blue field, a red rectangle at a known place in it, and a green one whose width follows the count. */
public class Swatch extends View {
    private final Paint paint = new Paint();
    int count;

    public Swatch(Context context, AttributeSet attrs) {
        super(context, attrs);
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
        MainActivity.draws++;
    }
}
