package megler.test.screen;

import android.app.Application;
import android.content.ComponentCallbacks;
import android.content.Context;
import android.content.res.Configuration;
import android.hardware.display.DisplayManager;
import android.util.Log;
import android.view.WindowManager;

/**
 * The Application: says what it hears of the screen's changes three ways, as an app hears them on Android -- its own
 * onConfigurationChanged, a ComponentCallbacks registered with it, and a display listener.
 */
public class App extends Application {
    static final String TAG = "ScreenTest";

    /** A configuration as the test reads it: its size in dp, its smallest width, its orientation and its density. */
    static String describe(Configuration c) {
        return "w=" + c.screenWidthDp + " h=" + c.screenHeightDp + " sw=" + c.smallestScreenWidthDp + " orientation=" + c.orientation + " dpi=" + c.densityDpi;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "App.onCreate " + describe(getResources().getConfiguration()));
        registerComponentCallbacks(new ComponentCallbacks() {
            @Override
            public void onConfigurationChanged(Configuration c) {
                Log.i(TAG, "App.callbacks " + describe(c));
            }

            @Override
            public void onLowMemory() {}
        });
        final DisplayManager displays = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        displays.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int id) {}

            @Override
            public void onDisplayRemoved(int id) {}

            @Override
            public void onDisplayChanged(int id) {
                android.view.Display d = ((WindowManager) getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
                android.graphics.Point size = new android.graphics.Point();
                d.getRealSize(size);
                Log.i(TAG, "App.display " + id + " rotation=" + d.getRotation() + " size=" + size.x + "x" + size.y);
            }
        }, null);
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        Log.i(TAG, "App.onConfigurationChanged " + describe(c));
    }
}
