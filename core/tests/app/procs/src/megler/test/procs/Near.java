package megler.test.procs;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

/** A service in the package's own process: it says its process as it is bound. */
public class Near extends Service {
    @Override
    public IBinder onBind(Intent i) {
        Log.i("ProcsTest", "near-service " + App.report(this));
        return new Binder();
    }
}
