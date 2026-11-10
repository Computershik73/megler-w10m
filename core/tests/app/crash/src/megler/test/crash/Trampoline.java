package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;

/**
 * Instagram's way in: First starts this and finishes, so this is the task's root (Activity.isTaskRoot: the lowest activity
 * not finishing). It starts Main twice with FLAG_ACTIVITY_CLEAR_TOP and FLAG_ACTIVITY_SINGLE_TOP, and stays: Main, not the
 * root, finishes in its onCreate, and the second start must not bring that finishing one back (onRestart, onStart). Then
 * Away, whose own top finishes their affinity, and once that is gone and this one is back, the activity that throws.
 */
public class Trampoline extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("CrashTest", "trampoline onCreate root=" + isTaskRoot());
        for (int i = 0; i < 2; i++)
            startActivity(new Intent(this, Main.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
        // (naming a referrer of its own by name, Intent.EXTRA_REFERRER_NAME, which Away's getReferrer gives back; a String
        // under Intent.EXTRA_REFERRER, where Android reads a Uri alone, is passed over)
        new Handler().post(() -> startActivity(new Intent(this, Away.class).putExtra(Intent.EXTRA_REFERRER, "https://not-a-uri.megler.test/")
                .putExtra(Intent.EXTRA_REFERRER_NAME, "android-app://megler.test.crash.trampoline")));
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!Away.sGone) return;
        // brought back by Away's top finishing their affinity, which this activity's (the package's) is not
        Log.i("CrashTest", "trampoline back after the affinity went");
        new Handler().post(() -> startActivity(new Intent(this, Second.class)));
    }
}
