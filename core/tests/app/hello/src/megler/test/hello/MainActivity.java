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
    private int presses;

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
}
