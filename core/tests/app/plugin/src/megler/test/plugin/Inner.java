package megler.test.plugin;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * A screen of the plug-in for itself alone (not exported), which its service opens (PluginService, asked to by the other
 * app; Total Commander's LAN plug-in opens its screens itself when its host does not for it): inside the app the service
 * runs in it is made as the plug-in's own, though no other app may open it. It opens another of the plug-in's screens by an
 * action with no package, as an app's code asks for its own, has its result, and finishes.
 */
public class Inner extends Activity {
    private static final String T = "PluginTest";
    static int created;
    /* What it was and what came back to it, for the service to tell (PluginService.STATE). */
    static volatile String said;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        created++;
        TextView v = new TextView(this);
        v.setText(R.string.inner);
        setContentView(v);
        PluginService service = PluginService.instance;
        String here = getPackageName() + "|" + getTitle() + "|" + (service != null && service.getApplication() == getApplication());
        Log.i(T, "inner created " + created + " " + here);
        said = here;
        startActivityForResult(new Intent(getPackageName() + ".PANEL"), 5);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Object note = data == null ? null : data.getParcelableExtra("note");
        said += "|" + request + "/" + result + "/" + (data == null ? null : data.getAction()) + "/" + note;
        Log.i(T, "inner heard " + said);
        finish();
    }
}
