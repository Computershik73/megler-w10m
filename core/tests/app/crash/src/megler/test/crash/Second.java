package megler.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/** Throws in onCreate. Nothing catches it: on Android the process is killed and the user is told the app has stopped. */
public class Second extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("CrashTest", "second onCreate, about to throw");
        throw new IllegalStateException("thrown in onCreate");
    }
}
