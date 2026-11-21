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
 * Main and Far: each says its process (App.report) and what its code finds of the security provider its process's Application
 * put first (App.providers). The first of them -- started from the launcher, or for another app's request -- binds both
 * services, sends the broadcast both receivers take, and starts the other for a result; the other takes its provider out of
 * its process's list, answers with the name of its process and finishes, and then the first, its own provider still there,
 * does, and the app ends.
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
        Log.i("ProcsTest", who() + " providers " + App.providers());
        // Contexts made from the activity's are its process's: their Application and class loader (the Google app's Gemini
        // screen asks the Application of a configuration context for its object graph).
        android.content.Context config = createConfigurationContext(new android.content.res.Configuration(getResources().getConfiguration()));
        android.content.Context own;
        try { own = createPackageContext(getPackageName(), 0); } catch (Exception e) { own = null; }
        Log.i("ProcsTest", who() + " derived config=" + App.which(config.getApplicationContext()) + " loader=" + (config.getClassLoader() == App.class.getClassLoader())
            + " package=" + (own == null ? "none" : App.which(own.getApplicationContext()) + " loader=" + (own.getClassLoader() == App.class.getClassLoader())));
        if (getCallingActivity() != null) {
            // (its process's provider taken out of its list: the first process's list keeps its own, onActivityResult)
            java.security.Security.removeProvider("ProcsTest");
            Log.i("ProcsTest", who() + " removed its provider: " + App.providers());
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
        if (request == 2) {
            Log.i("ProcsTest", who() + " heard Broken: " + result + " " + data);
            finish();
            return;
        }
        Log.i("ProcsTest", who() + " heard " + other().getSimpleName() + ": " + result + " from " + (data == null ? null : data.getStringExtra("process")));
        Log.i("ProcsTest", who() + " providers after " + other().getSimpleName() + " removed its: " + App.providers());
        startActivityForResult(new Intent(this, Broken.class), 2);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("ProcsTest", who() + " onDestroy");
    }
}
