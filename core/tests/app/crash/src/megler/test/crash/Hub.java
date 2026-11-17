package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;

/**
 * Of launch mode singleTask, started by Trampoline once Away's affinity went: it starts Over, which starts it again with no
 * flag at all, as Viber starts its RegistrationActivity again and again. As on Android, the one on the stack is brought
 * back with the intent (onRestart, onStart, onNewIntent, onResume) and Over goes, and no second one is made (the layer made
 * one at every start, four of Viber's in one session). Then it starts the activity that throws.
 */
public class Hub extends Activity {
    static int sCreations;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        sCreations++;
        Log.i("CrashTest", "hub onCreate creations=" + sCreations);
        // (A second one, made where the first should have been brought back, goes on to the end, not round again.)
        new Handler().post(() -> startActivity(new Intent(this, sCreations == 1 ? Over.class : Second.class)));
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.i("CrashTest", "hub onRestart");
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Log.i("CrashTest", "hub onNewIntent " + intent.getStringExtra("which") + " creations=" + sCreations);
        new Handler().post(() -> startActivity(new Intent(this, Second.class)));
    }
}
