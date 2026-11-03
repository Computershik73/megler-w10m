package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * Shows a line of text, and once it is on the screen starts the activity that throws. Before that, from its first onResume,
 * it starts Gate for a result, which gives it at once: what it hears meanwhile is said in order once it has the focus.
 * And in between, FLAG_ACTIVITY_CLEAR_TOP: it starts Top, which starts it again with that flag (Top's step "standard"),
 * and Android finishes this one and makes another; that one starts Top again, which starts it with FLAG_ACTIVITY_SINGLE_TOP
 * as well (step "single"), and it stays and hears onNewIntent; then it starts the activity that throws.
 * Before Top, it waits for Gate to be gone, its main thread idle between looks: a finished activity is destroyed once the
 * one below is drawn and the main thread is idle, or ten seconds on, and the steps after Gate kept the main thread busy
 * and ended the app, by the activity that throws, sooner than that.
 */
public class First extends Activity {
    private static int creations;
    private boolean started, gated;
    private String step;
    private final StringBuilder order = new StringBuilder();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        creations++;
        step = getIntent().getStringExtra("step");
        TextView text = new TextView(this);
        text.setText("the next screen throws in onCreate");
        setContentView(text);
        if (step != null) {
            // started again by Top through FLAG_ACTIVITY_CLEAR_TOP: no gate this time
            gated = true;
            Log.i("CrashTest", "first onCreate " + step + " creations=" + creations);
            return;
        }
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
    protected void onResume() {
        super.onResume();
        order.append("resume,");
        if (!gated) {
            gated = true;
            // through an alias of Gate's, which Gate recreates itself under (Play Store's main activity was started through its launcher's)
            startActivityForResult(new Intent().setClassName(getPackageName(), "megler.test.crash.GateAlias"), 7);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        order.append("result:").append(request).append(':').append(result).append(',');
        // only Gate's result is asked for; Android gives no other (the layer once gave one to an activity CLEAR_TOP brought back)
        if (request != 7) Log.i("CrashTest", "first unexpected result " + request + ":" + result);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Log.i("CrashTest", "first onNewIntent " + intent.getStringExtra("step") + " creations=" + creations);
        // the way into Instagram: the activity it starts is the task's root once this one finishes (Trampoline)
        startActivity(new Intent(this, Trampoline.class));
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "first onDestroy " + step);
    }

    private void whenGateGone(Runnable next, int waited) {
        if (Gate.sDestroyed || waited >= 15000) {
            next.run();
            return;
        }
        new android.os.Handler().postDelayed(() -> whenGateGone(next, waited + 100), 100);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        order.append("focus:").append(focus).append(',');
        if (focus && !started) {
            started = true;
            if (step == null) {
                Log.i("CrashTest", "first order " + order);
                whenGateGone(() -> startActivity(new Intent(this, Top.class).putExtra("step", "standard")), 0);
            } else {
                startActivity(new Intent(this, Top.class).putExtra("step", "single"));
            }
        }
    }
}
