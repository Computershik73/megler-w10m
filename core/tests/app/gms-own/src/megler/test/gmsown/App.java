package megler.test.gmsown;

import android.app.Application;
import android.content.Context;
import android.util.Log;

/**
 * Their Application, attached slowly as GmsCore's is (its attachBaseContext sets up its flags and module configuration: a second
 * and more on a phone). The test app gms-host asks their provider from a thread that holds a lock of its own meanwhile, as
 * Smash Hit's GoogleApiHandler asked their Chimera provider with its GmsClientSupervisor's lock held (Main.askUnderLock).
 */
public class App extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try { Thread.sleep(2000); } catch (InterruptedException e) { }
        Log.i("GmsOwnTest", "application attached");
    }
}
