package megler.test.procs;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** A receiver of the manifest in the process ":far". */
public class FarReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        Log.i("ProcsTest", "far-receiver process=" + android.app.Application.getProcessName());
    }
}
