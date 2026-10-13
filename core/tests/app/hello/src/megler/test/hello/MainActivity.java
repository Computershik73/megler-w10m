package megler.test.hello;

import android.app.Activity;
import android.os.Bundle;
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
