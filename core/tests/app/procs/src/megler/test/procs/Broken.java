package megler.test.procs;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/**
 * The activity in the process ":broken", whose Application throws as it is made (App.onCreate): on Android that process
 * crashes as it starts, this activity is never made, and the one that asked for it hears RESULT_CANCELED.
 */
public class Broken extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("ProcsTest", "broken made in " + android.app.Application.getProcessName());
        finish();
    }
}
