package megler.test.plugin;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** The plug-in's launcher activity: what its tile opens, and not what another app's request for Shown opens. */
public class Main extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("PluginTest", "main onCreate");
        TextView v = new TextView(this);
        v.setText("Plug-in");
        setContentView(v);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("PluginTest", "main onDestroy");
    }
}
