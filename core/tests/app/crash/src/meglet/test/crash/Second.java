package meglet.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/**
 * Throws in onCreate. Nothing catches it: on Android the process is killed and the user is told the app has stopped. The exception has a cause that has a cause,
 * the innermost with a message far too long for a dialog: the layer's sentence must name the first and the last and be short.
 */
public class Second extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("CrashTest", "second onCreate, about to throw");
        throw new IllegalStateException("thrown in onCreate", new RuntimeException("a cause in between", new java.io.IOException("the innermost cause, which says what went wrong, at length: " + new String(new char[200]).replace((char) 0, 'x'))));
    }
}
