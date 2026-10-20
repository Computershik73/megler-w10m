package meglet.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.widget.TextView;

/**
 * Started over First, it starts First again with FLAG_ACTIVITY_CLEAR_TOP, as Instagram goes back to its signed-out screen:
 * step "standard" with that flag alone (Android finishes First and makes a new one), step "single" with
 * FLAG_ACTIVITY_SINGLE_TOP as well (First stays and hears onNewIntent). Either way this one goes.
 */
public class Top extends Activity {
    private String step;
    private boolean asked;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        step = getIntent().getStringExtra("step");
        TextView text = new TextView(this);
        text.setText("top " + step);
        setContentView(text);
        Log.i("CrashTest", "top onCreate " + step);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (asked) return;
        asked = true;
        new Handler().post(() -> {
            boolean single = "single".equals(step);
            Intent back = new Intent(this, First.class).putExtra("step", single ? "third" : "again");
            back.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | (single ? Intent.FLAG_ACTIVITY_SINGLE_TOP : 0));
            startActivity(back);
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "top onDestroy " + step);
    }
}
