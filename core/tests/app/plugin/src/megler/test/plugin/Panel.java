package megler.test.plugin;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.widget.TextView;

import megler.test.shared.Note;

/**
 * A screen of the plug-in the other app opens while the plug-in's service runs inside it (the test app crossapp; Total
 * Commander opens its LAN plug-in's ConnectActivity so, "New server"): it runs inside that app too (megler.HostedApps), as
 * the plug-in's code -- its package, its resources, its Application, the service's own instance, which that activity begins
 * with. It says what it got, tells the service by a broadcast within the package, as that activity tells of the server it
 * saved, and once it is on the screen finishes with a result. Its manifest gives it no title and does not say whether other
 * apps may open it: the plug-in's label, and they may, for it has a filter.
 */
public class Panel extends Activity {
    private static final String T = "PluginTest";
    static int created;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        created++;
        TextView v = new TextView(this);
        v.setText(R.string.app_name);
        setContentView(v);
        Object note = getIntent().getParcelableExtra("note");
        PluginService service = PluginService.instance;
        boolean one = service != null && service.getApplication() == getApplication();
        Log.i(T, "panel created " + created + " package=" + getPackageName() + " title=" + getTitle() + " note=" + note + " mine=" + (note instanceof Note)
                 + " application=" + App.created + " service=" + one);
        sendBroadcast(new Intent(PluginService.SAVED).setPackage(getPackageName()).putExtra("name", "panel " + created));
        setResult(RESULT_OK, new Intent(getPackageName() + ".PANEL_DONE").putExtra("note", new Note(10, "ten", null, null, null))
                  .putExtra("where", getPackageName() + "|" + getTitle() + "|" + getComponentName().flattenToShortString() + "|" + one));
    }

    @Override
    protected void onResume() {
        super.onResume();
        new Handler().post(this::finish);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "panel onDestroy");
    }
}
