package megler.test.data;

/**
 * The data app's Application. While it is created, a thread of its own asks for the process's Application as Tuya Smart's
 * library loader asks on a worker thread while its Application starts (ThingLibraryLoader.getContext): Android's hidden
 * ActivityThread.currentActivityThread() and its field mInitialApplication, and currentApplication(), by reflection. Main's
 * step "activity-thread-application" looks at what it found.
 */
public class DataApplication extends android.app.Application {
    /** What the thread found while onCreate ran. */
    static volatile String seenInOnCreate = "not run";

    @Override
    public void onCreate() {
        super.onCreate();
        final String[] seen = { "not seen" };
        final android.app.Application self = this;
        Thread t = new Thread(() -> {
            try {
                Class<?> at = Class.forName("android.app.ActivityThread");
                Object thread = at.getMethod("currentActivityThread").invoke(null);
                java.lang.reflect.Field f = at.getDeclaredField("mInitialApplication");
                f.setAccessible(true);
                Object field = f.get(thread);
                Object current = at.getMethod("currentApplication").invoke(null);
                seen[0] = "field " + (field == self) + " current " + (current == self);
            } catch (Throwable e) {
                seen[0] = String.valueOf(e);
            }
        }, "DataApplication-probe");
        t.start();
        try {
            t.join(10000);
        } catch (InterruptedException e) {
            seen[0] = String.valueOf(e);
        }
        seenInOnCreate = seen[0];
    }
}
