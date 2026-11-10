package megler.test.gmsown;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

/** Google Play services' location API, as an app's FusedLocationProviderClient binds it: a binder, and a line when it is bound. */
public class LocationApi extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        Log.i("GmsOwnTest", "location api bound");
        return new Binder();
    }
}
