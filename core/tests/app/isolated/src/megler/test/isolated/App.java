package megler.test.isolated;

import android.app.Application;

/** The base's Application, as Chrome's SplitChromeApplication is in its base: it counts its start in a class of the base. */
public class App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        Counter.started++;
    }
}
