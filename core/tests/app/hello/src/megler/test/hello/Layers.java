package megler.test.hello;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RecordingCanvas;
import android.graphics.RenderNode;
import android.util.Log;
import android.view.View;

/**
 * A view that draws as Compose's view does: what changes is in RenderNodes it keeps -- a bar, a square, and a disc drawn in the
 * square's place now and then -- recorded apart from its own drawing, which draws them by reference; and whenever one of them
 * changes, the view invalidates itself whole. Its own drawing below them, a green field, never changes. Started, it changes them
 * every 40 ms, STEPS times: the bar's node recorded again with its yellow part longer, the square moved along by its translation
 * and made translucent every other time, the disc drawn instead of it every tenth time. Each frame drawn in part is checked
 * against the screen drawn whole (megler.ViewRoot.checkPart), and none may reach the green field: what changed is found node by
 * node and call by call (megler.Gfx.damage), not taken to be the whole view.
 */
public class Layers extends View {
    static final int STEPS = 30;
    private final RenderNode bar = new RenderNode("bar"), square = new RenderNode("square"), disc = new RenderNode("disc");
    private final Paint paint = new Paint();
    private int step;

    public Layers(Context context) {
        super(context);
    }

    void start() {
        Log.i("HelloTest", "layers started");
        post(tick);
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            step++;
            recordBar();
            square.setTranslationX(4 * step);
            square.setAlpha(step % 2 == 0 ? 1f : 0.5f);
            invalidate();
            if (step < STEPS) postDelayed(this, 40);
            else Log.i("HelloTest", "layers steps=" + step);
        }
    };

    private void recordBar() {
        int w = getWidth();
        bar.setPosition(0, 0, w, 10);
        RecordingCanvas c = bar.beginRecording();
        paint.setColor(0xFF2050A0);
        c.drawRect(0, 0, w, 10, paint);
        paint.setColor(0xFFE0A000);
        c.drawRect(0, 0, 4 * step, 10, paint);
        bar.endRecording();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        recordBar();
        square.setPosition(0, 20, 20, 40);
        RecordingCanvas c = square.beginRecording();
        paint.setColor(0xFF00A0A0);
        c.drawRect(0, 0, 20, 20, paint);
        square.endRecording();
        disc.setPosition(0, 20, 20, 40);
        c = disc.beginRecording();
        paint.setColor(0xFFA000A0);
        c.drawOval(0, 0, 20, 20, paint);
        disc.endRecording();
    }

    @Override
    protected void onDraw(Canvas c) {
        paint.setColor(0xFF40A040);
        c.drawRect(0, 60, getWidth(), getHeight(), paint);
        if (!bar.hasDisplayList()) return;
        c.drawRenderNode(bar);
        c.drawRenderNode(step % 10 == 9 ? disc : square);
    }
}
