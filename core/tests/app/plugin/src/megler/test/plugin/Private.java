package megler.test.plugin;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

/** The plug-in's service for itself alone (not exported): its own PluginService binds it and has its Binder as the class it is. */
public class Private extends Service {
    final class Local extends Binder {
        Private service() { return Private.this; }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return new Local();
    }

    @Override
    public void onDestroy() {
        Log.i("PluginTest", "its own service destroyed");
        super.onDestroy();
    }
}
