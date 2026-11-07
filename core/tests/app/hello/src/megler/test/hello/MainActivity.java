package megler.test.hello;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/** The launcher activity: a layout from resources, a button whose listener changes a text, and a line in the log at every step of its life. */
public class MainActivity extends Activity {
    static int draws;
    private int presses, resumes;
    private Layers layers;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("HelloTest", "onCreate app=" + App.created + " state=" + (state == null ? "none" : "saved"));
        setContentView(R.layout.main);
        final TextView text = (TextView) findViewById(R.id.text);
        final Swatch swatch = (Swatch) findViewById(R.id.swatch);
        Button button = (Button) findViewById(R.id.button);
        Log.i("HelloTest", "views text=\"" + text.getText() + "\" button=\"" + button.getText() + "\"");
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                presses++;
                text.setText(getString(R.string.pressed, presses));
                swatch.setCount(presses);
                Log.i("HelloTest", "onClick presses=" + presses + " text=\"" + text.getText() + "\"");
            }
        });
        /* A message copied and the copy sent, which is how a support-library dialog sends its buttons' messages: the copy says what the first says, to the same handler. */
        Handler handler = new Handler() {
            @Override
            public void handleMessage(Message m) {
                Log.i("HelloTest", "message what=" + m.what + " arg=" + m.arg1 + "," + m.arg2 + " obj=" + m.obj + " when=" + (m.getWhen() > 0));
            }
        };
        Message first = handler.obtainMessage(7, 1, 2, "word");
        Message copy = Message.obtain(first);
        Log.i("HelloTest", "message copy=" + (copy != null && copy != first && copy.getTarget() == handler));
        if (copy != null) copy.sendToTarget();
        /*
         * Two views with no width and a weight each, asking for 40 and for 100, in a row with no limit on its width: this app is written for
         * Android 9, and Android gives such an app's two the same width, half of the 140. With an exact 200 they have 100 each.
         */
        android.widget.LinearLayout row = new android.widget.LinearLayout(this);
        android.view.View narrow = new android.view.View(this), wide = new android.view.View(this);
        narrow.setMinimumWidth(40);
        wide.setMinimumWidth(100);
        row.addView(narrow, new android.widget.LinearLayout.LayoutParams(0, 10, 1));
        row.addView(wide, new android.widget.LinearLayout.LayoutParams(0, 10, 1));
        row.measure(0, 0);
        String unlimited = row.getMeasuredWidth() + ":" + narrow.getMeasuredWidth() + "+" + wide.getMeasuredWidth();
        row.measure(android.view.View.MeasureSpec.makeMeasureSpec(200, android.view.View.MeasureSpec.EXACTLY), 0);
        Log.i("HelloTest", "row unlimited=" + unlimited + " exact=" + row.getMeasuredWidth() + ":" + narrow.getMeasuredWidth() + "+" + wide.getMeasuredWidth());
        /*
         * Two surfaces over two views in the bottom right corner: one that nothing draws, which must leave the green view under it to be
         * seen (Android clears a surface's area only once it has a frame), and one drawn magenta, whose frame must be shown.
         */
        android.widget.FrameLayout corner = new android.widget.FrameLayout(this);
        View green = new View(this), blue = new View(this);
        green.setBackgroundColor(0xFF00A000);
        blue.setBackgroundColor(0xFF0000C0);
        android.view.SurfaceView blank = new android.view.SurfaceView(this), drawn = new android.view.SurfaceView(this);
        corner.addView(green, new android.widget.FrameLayout.LayoutParams(40, 40, android.view.Gravity.START));
        corner.addView(blank, new android.widget.FrameLayout.LayoutParams(40, 40, android.view.Gravity.START));
        corner.addView(blue, new android.widget.FrameLayout.LayoutParams(40, 40, android.view.Gravity.END));
        corner.addView(drawn, new android.widget.FrameLayout.LayoutParams(40, 40, android.view.Gravity.END));
        drawn.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(android.view.SurfaceHolder holder) {
                android.graphics.Canvas c = holder.lockCanvas();
                if (c == null) return;
                c.drawColor(0xFFC000C0);
                holder.unlockCanvasAndPost(c);
                Log.i("HelloTest", "surface frame posted");
            }

            @Override
            public void surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(android.view.SurfaceHolder holder) {}
        });
        addContentView(corner, new android.widget.FrameLayout.LayoutParams(80, 40, android.view.Gravity.BOTTOM | android.view.Gravity.END));
        /* A view that draws as Compose's does, in the bottom left corner: its nodes change once the app is shown again (Layers). */
        layers = new Layers(this);
        addContentView(layers, new android.widget.FrameLayout.LayoutParams(200, 120, android.view.Gravity.BOTTOM | android.view.Gravity.START));
        /* A launcher's questions: the platform's padding around a widget's frame, and the activities there are to list -- this app's own, by its label. */
        android.graphics.Rect padding = android.appwidget.AppWidgetHostView.getDefaultPaddingForWidget(this, getComponentName(), null);
        android.content.pm.LauncherApps apps = (android.content.pm.LauncherApps) getSystemService(LAUNCHER_APPS_SERVICE);
        java.util.List<android.content.pm.LauncherActivityInfo> launchable = apps.getActivityList(getPackageName(), android.os.Process.myUserHandle());
        android.content.pm.LauncherActivityInfo own = launchable.isEmpty() ? null : launchable.get(0);
        Log.i("HelloTest", "launcher padding=" + padding);
        Log.i("HelloTest", "launcher activities=" + launchable.size() + (own == null ? "" : " first=" + own.getComponentName().getClassName() + " label=" + own.getLabel() + " icon="
                + (own.getIcon(0) != null) + " resolved=" + (apps.resolveActivity(getIntent(), own.getUser()) != null)));
        /* A switch of the theme's, measured with nothing to constrain it: its width is made of its thumb and its track's padding, not of the track picture's own width. */
        android.widget.Switch sw = new android.widget.Switch(this);
        sw.measure(0, 0);
        android.graphics.Rect thumbPad = new android.graphics.Rect(), trackPad = new android.graphics.Rect();
        android.graphics.drawable.Drawable thumb = sw.getThumbDrawable(), track = sw.getTrackDrawable();
        if (thumb != null) thumb.getPadding(thumbPad);
        if (track != null) track.getPadding(trackPad);
        Log.i("HelloTest", "switch width=" + sw.getMeasuredWidth() + " height=" + sw.getMeasuredHeight() + " thumb=" + (thumb == null ? -1 : thumb.getIntrinsicWidth() - thumbPad.left - thumbPad.right)
                + " trackpad=" + (trackPad.left + trackPad.right) + " track=" + (track == null ? -1 : track.getIntrinsicWidth()));
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i("HelloTest", "onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i("HelloTest", "onResume");
        if (++resumes == 2 && layers != null) layers.start();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        View root = findViewById(R.id.root);
        View button = findViewById(R.id.button);
        View swatch = findViewById(R.id.swatch);
        int[] at = new int[2];
        button.getLocationOnScreen(at);
        Log.i("HelloTest", "layout focus=" + focus + " root=" + root.getWidth() + "x" + root.getHeight() + " button=" + at[0] + "," + at[1] + "," + button.getWidth() + "x"
                + button.getHeight());
        swatch.getLocationOnScreen(at);
        Log.i("HelloTest", "layout swatch=" + at[0] + "," + at[1] + "," + swatch.getWidth() + "x" + swatch.getHeight());
        View mark = findViewById(R.id.mark);
        View text = findViewById(R.id.text);
        mark.getLocationOnScreen(at);
        Log.i("HelloTest", "layout mark=" + at[0] + "," + at[1] + "," + mark.getWidth() + "x" + mark.getHeight());
        text.getLocationOnScreen(at);
        Log.i("HelloTest", "layout text=" + at[0] + "," + at[1] + "," + text.getWidth() + "x" + text.getHeight());
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.i("HelloTest", "onPause draws=" + (draws > 0));
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i("HelloTest", "onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("HelloTest", "onDestroy presses=" + presses);
    }

    /*
     * Back as React Native's and Flutter's activities take it: handed on, and the platform's Back -- the task to the back,
     * this being its root -- done a while later: the first time later than the shell waits for it, then soon.
     */
    private int backs;
    @Override
    public void onBackPressed() {
        final int delay = ++backs == 1 ? 900 : 150;
        Log.i("HelloTest", "back " + backs + " handed on for " + delay + " ms");
        new Handler().postDelayed(new Runnable() {
            @Override
            public void run() {
                MainActivity.super.onBackPressed();
            }
        }, delay);
    }
}
