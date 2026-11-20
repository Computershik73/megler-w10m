package megler.test.procs;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.widget.TextView;

/**
 * Main and Far: each says its process (App.report). The first of them -- started from the launcher, or for another app's
 * request -- binds both services, sends the broadcast both receivers take, and starts the other for a result; the other
 * answers with the name of its process and finishes, and then the first does, and the app ends.
 */
abstract class Shown extends Activity {
    abstract String who();
    abstract Class<? extends Activity> other();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView v = new TextView(this);
        v.setText(who());
        setContentView(v);
        // (the component it was started as: an alias of it that another app's intent reached, as on Android)
        Log.i("ProcsTest", who() + " intent " + getIntent().getComponent().getClassName() + " via=" + getIntent().getStringExtra("via"));
        Log.i("ProcsTest", who() + " " + App.report(this));
        if (getCallingActivity() != null) {
            setResult(RESULT_OK, new Intent().putExtra("process", android.app.Application.getProcessName()));
            finish();
            return;
        }
        bind(Near.class);
        bind(FarService.class);
        sendBroadcast(new Intent("megler.test.procs.PING").setPackage(getPackageName()));
        startActivityForResult(new Intent(this, other()), 1);
    }

    private void bind(Class<?> service) {
        bindService(new Intent(this, service), new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder b) {
                Log.i("ProcsTest", who() + " bound " + name.getShortClassName());
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
        }, BIND_AUTO_CREATE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        Log.i("ProcsTest", who() + " heard " + other().getSimpleName() + ": " + result + " from " + (data == null ? null : data.getStringExtra("process")));
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("ProcsTest", who() + " onDestroy");
    }
}
