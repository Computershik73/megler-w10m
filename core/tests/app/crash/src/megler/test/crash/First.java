package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** Shows a line of text, and once it is on the screen starts the activity that throws. */
public class First extends Activity {
    private boolean started;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("the next screen throws in onCreate");
        setContentView(text);
        Log.i("CrashTest", "first onCreate");
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus && !started) {
            started = true;
            startActivity(new Intent(this, Second.class));
        }
    }
}
