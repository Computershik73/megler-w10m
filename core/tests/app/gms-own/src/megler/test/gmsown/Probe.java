package megler.test.gmsown;

import android.app.Application;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * A service of Google Play services' package, run where an app hosts them: what their code is told of its process. Google's
 * own take the process whose name is their application's process name for their main one (GmsModuleFinder: only there is
 * their module configuration made again at once; elsewhere a scan that finds a change asks for another, without end). Hosted
 * in an app, their application runs in the app's process, which Application.getProcessName names to their code.
 */
public class Probe extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int id) {
        String application = getApplicationInfo().processName, current = Application.getProcessName();
        Log.i("GmsOwnTest", "main process " + (application != null && application.equals(current)) + " application=" + application + " current=" + current);
        stopSelf(id);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
