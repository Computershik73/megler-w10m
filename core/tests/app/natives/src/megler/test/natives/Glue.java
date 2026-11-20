package megler.test.natives;

import android.app.NativeActivity;
import android.os.Bundle;
import android.util.Log;

/**
 * A NativeActivity of the app's own, as KOReader's MainActivity is: its library (jni/natives_activity.c, android.app.lib_name
 * "natives_activity") is started by the framework's NativeActivity and asks this class's own methods through JNI on clazz.
 * Started by Main at the tap after the stack's; Back, which its native code leaves unhandled, finishes it.
 */
public final class Glue extends NativeActivity {
    private static final String T = "NativesActivity";

    @Override
    protected void onCreate(Bundle state) {
        Log.i(T, "Java onCreate");
        super.onCreate(state);
    }

    /** Asked by the native code, on the main thread and from its own. */
    public int answer(int x) { return 2 * x + 1; }
    public String greeting() { return "hello from " + getClass().getSimpleName(); }

    @Override
    public void onBackPressed() {
        Log.i(T, "Java onBackPressed");
        super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "Java onDestroy");
    }
}
