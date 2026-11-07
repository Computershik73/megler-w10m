// Megler's own (the original has no such class: its screen kept the size it started with). The app's screen changing under it,
// made what Android makes it: a configuration change.
//
// The shell resizes the app's surface (a PC's window) or turns it (a phone's), megler.h's megler_set_surface; the core tells the
// main loop with an event of its own (EV_SURFACE, core/src/drivers/mg_java.c), and AppRunner.deliver hands it here, where
// ActivityThread has the window manager's new configuration:
//  - every Resources of the app's takes the screen's new orientation, sizes, layout and density, and DisplayMetrics of the new
//    size (Resources.meglerScreenChanged); the rest of their configuration stays, an app's own locale with it;
//  - the Application, the callbacks registered with it or with a context, the running services and the content providers hear
//    onConfigurationChanged (ActivityThread.handleConfigurationChanged);
//  - each activity on the screen meets the change now, the others as they come back (AppRunner.ensureConfiguration): one whose
//    android:configChanges takes all that changed hears onConfigurationChanged, the others are made again with the state they
//    saved (AppRunner.relaunch, as ActivityThread.handleRelaunchActivity);
//  - every window is laid out again at the new size, its views hearing the change (ViewRoot.screenChanged);
//  - DisplayManager's listeners hear onDisplayChanged.
// A change of size relaunches only an activity whose resources have a size it crosses (relaunchFor), as Android's
// SizeConfigurationBuckets keeps the sizes an app's resources are made for: a window resized on a PC from 400 to 600 dp wide
// relaunches an app with layouts for w600dp, and not one with none. A change of orientation is always one. The rotation alone
// (a phone turned upside down) changes no configuration: the display's listeners hear it, and Display.getRotation says it.
package megler;

import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;

/** The screen's size, density and rotation as the app has them, and their changes as configuration changes. */
public final class Screen {
    private Screen() {}

    /** The configuration changes a change of the screen can bring: ActivityInfo.CONFIG_ORIENTATION, _SCREEN_LAYOUT, _SCREEN_SIZE, _SMALLEST_SCREEN_SIZE, _DENSITY. */
    public static final int CONFIG_ORIENTATION = 0x80, CONFIG_SCREEN_LAYOUT = 0x100, CONFIG_SCREEN_SIZE = 0x400, CONFIG_SMALLEST_SCREEN_SIZE = 0x800,
        CONFIG_DENSITY = 0x1000;

    /* The screen as the app was last told it, in pixels; -1 before the app started. */
    private static int sWidth = -1, sHeight, sDpi, sRotation;

    /** The app starts on the screen as it is now (AppRunner.run). */
    static void started() {
        sWidth = Native.screenWidth();
        sHeight = Native.screenHeight();
        sDpi = dpi();
        sRotation = Native.displayRotation();
    }

    private static int dpi() { return Math.round(Native.density() * 160); }

    /** The process's configuration now, the app's resources': what a new activity starts with and what the others meet. */
    public static Configuration configuration() { return ContextImpl.app().getResources().getConfiguration(); }

    /** The screen's fields of the device's configuration now, every other field undefined: what Configuration.updateFrom takes of it. */
    static Configuration screenNow() {
        Configuration device = Configuration.meglerDevice(), screen = new Configuration();
        screen.orientation = device.orientation;
        screen.screenWidthDp = device.screenWidthDp;
        screen.screenHeightDp = device.screenHeightDp;
        screen.smallestScreenWidthDp = device.smallestScreenWidthDp;
        screen.densityDpi = device.densityDpi;
        screen.screenLayout = device.screenLayout & ~Configuration.SCREENLAYOUT_LAYOUTDIR_MASK;
        return screen;
    }

    /** What changed of the screen between two configurations (CONFIG_*): its fields alone, whatever else differs. */
    public static int changes(Configuration from, Configuration to) {
        int c = 0;
        if (from.orientation != to.orientation) c |= CONFIG_ORIENTATION;
        if ((from.screenLayout & ~Configuration.SCREENLAYOUT_LAYOUTDIR_MASK) != (to.screenLayout & ~Configuration.SCREENLAYOUT_LAYOUTDIR_MASK)) c |= CONFIG_SCREEN_LAYOUT;
        if (from.screenWidthDp != to.screenWidthDp || from.screenHeightDp != to.screenHeightDp) c |= CONFIG_SCREEN_SIZE;
        if (from.smallestScreenWidthDp != to.smallestScreenWidthDp) c |= CONFIG_SMALLEST_SCREEN_SIZE;
        if (from.densityDpi != to.densityDpi) c |= CONFIG_DENSITY;
        return c;
    }

    /**
     * Of what changed, what makes an activity that does not take it be made again, as Android's SizeConfigurationBuckets.filterDiff:
     * the size, the smallest size and the screen layout only where a size the app's resources are made for lies between the
     * two (Resources.meglerSizeBuckets); the orientation and the density always.
     */
    public static int relaunchFor(int changes, Configuration from, Configuration to) {
        int[][] b = Resources.meglerSizeBuckets();
        if ((changes & CONFIG_SCREEN_SIZE) != 0 && !crosses(b[0], from.screenWidthDp, to.screenWidthDp) && !crosses(b[1], from.screenHeightDp, to.screenHeightDp))
            changes &= ~CONFIG_SCREEN_SIZE;
        if ((changes & CONFIG_SMALLEST_SCREEN_SIZE) != 0 && !crosses(b[2], from.smallestScreenWidthDp, to.smallestScreenWidthDp)) changes &= ~CONFIG_SMALLEST_SCREEN_SIZE;
        if ((changes & CONFIG_SCREEN_LAYOUT) != 0) {
            int sizeAndLong = Configuration.SCREENLAYOUT_SIZE_MASK | Configuration.SCREENLAYOUT_LONG_MASK | Configuration.SCREENLAYOUT_LAYOUTDIR_MASK;
            boolean others = (from.screenLayout & ~sizeAndLong) != (to.screenLayout & ~sizeAndLong);
            boolean size = layoutSizeCrosses(b[3][0] != 0, from.screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK, to.screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK);
            boolean lng = b[3][1] != 0 && (from.screenLayout & Configuration.SCREENLAYOUT_LONG_MASK) != (to.screenLayout & Configuration.SCREENLAYOUT_LONG_MASK);
            if (!others && !size && !lng) changes &= ~CONFIG_SCREEN_LAYOUT;
        }
        return changes;
    }

    /* As SizeConfigurationBuckets.crossesSizeThreshold: a size the resources are made for lies at or below one of the two and above the other. */
    private static boolean crosses(int[] sizes, int a, int b) {
        for (int s : sizes) if ((a < s && b >= s) || (a >= s && b < s)) return true;
        return false;
    }

    /*
     * As crossesScreenLayoutSizeThreshold: a smaller screen layout size always crosses (Android counts on resources for a normal
     * screen or less), a larger one only for an app whose resources name a size (`named`: layout-large and the like).
     */
    private static boolean layoutSizeCrosses(boolean named, int from, int to) {
        if (from == to) return false;
        if (to < from) return true;
        return named;
    }

    /**
     * The surface changed (EV_SURFACE, from AppRunner.deliver; main thread): see the top of the file. Nothing happens when it is
     * as the app was last told it.
     */
    public static void changed() {
        int w = Native.screenWidth(), h = Native.screenHeight(), dpi = dpi(), rotation = Native.displayRotation();
        if (sWidth < 0) started();
        if (w == sWidth && h == sHeight && dpi == sDpi && rotation == sRotation) return;
        android.util.Log.i("Megler", "the screen is " + w + "x" + h + " at " + dpi + " dpi, turned " + rotation * 90 + " degrees (it was " + sWidth + "x" + sHeight + " at "
                           + sDpi + " dpi, " + sRotation * 90 + ")");
        sWidth = w;
        sHeight = h;
        sDpi = dpi;
        sRotation = rotation;
        Configuration was = new Configuration(configuration()), screen = screenNow();
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        Resources.meglerScreenChanged(screen, metrics);
        Configuration now = new Configuration(configuration());
        int changes = changes(was, now);
        if (changes != 0) {
            android.util.Log.i("Megler", "configuration changes 0x" + Integer.toHexString(changes) + ": " + now);
            tellComponents(now);
        }
        java.util.List<ViewRoot> windows = new java.util.ArrayList<>(ViewRoot.roots());
        AppRunner.screenChanged();
        ViewRoot.screenChanged(now, changes != 0, windows);
        android.hardware.display.DisplayManager.meglerDisplayChanged();
    }

    /* The application, the callbacks registered with it and with contexts, the running services and the providers: onConfigurationChanged. */
    private static void tellComponents(Configuration now) {
        android.app.Application app = AppRunner.application();
        if (app != null) {
            try { app.onConfigurationChanged(new Configuration(now)); }
            catch (RuntimeException e) { android.util.Log.e("Megler", "the application's onConfigurationChanged threw", e); throw e; }
        }
        ContextImpl.app().meglerConfigurationChanged(new Configuration(now));
        for (int i = 0; i < AppRunner.stackSize(); i++) {
            android.content.Context base = AppRunner.activityAt(i).getBaseContext();
            if (base instanceof ContextImpl && base != ContextImpl.app()) ((ContextImpl) base).meglerConfigurationChanged(new Configuration(now));
        }
        for (android.app.Service s : Services.running()) s.onConfigurationChanged(new Configuration(now));
        for (android.content.ContentProvider p : android.content.ContentResolver.meglerProviders()) p.onConfigurationChanged(new Configuration(now));
    }
}
