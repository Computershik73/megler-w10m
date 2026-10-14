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
        /*
         * This app says it is written for Android 6 (targetSdkVersion 23). Two views with no width and a weight each, asking for 40 and for
         * 100, in a row with no limit on its width: Android leaves such an app's two what they asked for, where an app for Android 7 or later
         * has them made equal (the hello app). With an exact 200 they have 100 each, as there.
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
        Log.i("CrashTest", "row unlimited=" + unlimited + " exact=" + row.getMeasuredWidth() + ":" + narrow.getMeasuredWidth() + "+" + wide.getMeasuredWidth());
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
