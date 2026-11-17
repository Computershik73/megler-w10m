package megler.test.plugin;

import android.app.Application;
import android.util.Log;

/** The plug-in's Application: made once wherever the plug-in's code runs, and it says so. */
public class App extends Application {
    static int created;

    @Override
    public void onCreate() {
        super.onCreate();
        created++;
        Log.i("PluginTest", "application created " + created + " package=" + getPackageName());
    }
}
