package megler.test.splits;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * A service of the app's other process (":other" in the manifest), which loads the app's native library, as Play Store's
 * background process loads its own to install an app: the library is only in the split for 32-bit ARM, and the process's
 * class loader is another one than the main process's. It says the library's answer and the class loader it ran in.
 */
public class Other extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String said;
        try {
            System.loadLibrary("splitlib");
            said = "answer=" + Main.answer();
        } catch (Throwable t) {
            said = "FAILED: " + t;
        }
        Log.i("SplitsTest", "other-process " + said + " loader=" + Other.class.getClassLoader().getClass().getName());
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
