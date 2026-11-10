package android.os;

/**
 * Megler: Android's hidden ServiceManager, which apps reach by reflection for the system's binders -- Shizuku's and Sui's
 * SystemServiceHelper asks it for "package" to find out whether Sui is there.
 *
 * Megler's system services are objects in the app's own process (Context.getSystemService), with no binders behind them. A
 * phone publishes its platform's own services under their names, though, and an app may count on one being there: the
 * names of those (SERVICES: the window manager, the activity manager, the package manager...) are answered with a binder
 * that stands for the service -- the same one each time, its interface's descriptor, alive, with no local interface (as a
 * binder of another process has none), and every transaction refused (Binder.onTransact's false, nothing in the reply), as a
 * service refuses a code it does not know. An app that goes on to the interface's stub (IWindowManager.Stub.asInterface)
 * finds no such class here, ClassNotFoundException, which such code catches, as it did before this class was there; Sui's
 * transaction on the binder brings no binder back, and Sui is not there. Any other name is a service that is not there, as
 * a service missing on a phone is: getService and checkService answer null, getServiceOrThrow throws
 * ServiceNotFoundException. listServices lists the platform's names; services added are not kept.
 *
 * Morphe Manager ended at its start without the class (0.1.0.673): SystemServiceHelper found no ServiceManager, kept no
 * Method for getService, and Sui.init invoked that null Method. And Instagram (453) ended when every name was a service
 * that is not there (0.1.0.681): with the navigation bar's inset 0 (a phone whose bar the user has hidden), its
 * WindowInsetsManager asks the window manager whether there is a navigation bar -- getService("window") cast to IBinder as
 * Kotlin casts, then IWindowManager.Stub.asInterface and hasNavigationBar() by reflection, falling back to the platform's
 * config_showNavigationBar when the reflection fails -- and the cast of null was a NullPointerException ("null cannot be
 * cast to non-null type android.os.IBinder"), which nothing caught, from its insets listener in the window's layout.
 */
public final class ServiceManager {
    private ServiceManager() {}

    /** What getServiceOrThrow throws, as Android's does. */
    public static class ServiceNotFoundException extends Exception {
        public ServiceNotFoundException(String name) {
            super("No service published for: " + name);
        }
    }

    /** The platform's own services, by the names a phone publishes them under, each with its interface's descriptor. */
    private static final String[][] SERVICES = {
        { "accessibility", "android.view.accessibility.IAccessibilityManager" },
        { "account", "android.accounts.IAccountManager" },
        { "activity", "android.app.IActivityManager" },
        { "activity_task", "android.app.IActivityTaskManager" },
        { "alarm", "android.app.IAlarmManager" },
        { "appops", "com.android.internal.app.IAppOpsService" },
        { "audio", "android.media.IAudioService" },
        { "batterystats", "com.android.internal.app.IBatteryStats" },
        { "bluetooth_manager", "android.bluetooth.IBluetoothManager" },
        { "clipboard", "android.content.IClipboard" },
        { "connectivity", "android.net.IConnectivityManager" },
        { "content", "android.content.IContentService" },
        { "device_policy", "android.app.admin.IDevicePolicyManager" },
        { "deviceidle", "android.os.IDeviceIdleController" },
        { "display", "android.hardware.display.IDisplayManager" },
        { "input", "android.hardware.input.IInputManager" },
        { "input_method", "com.android.internal.view.IInputMethodManager" },
        { "iphonesubinfo", "com.android.internal.telephony.IPhoneSubInfo" },
        { "isub", "com.android.internal.telephony.ISub" },
        { "jobscheduler", "android.app.job.IJobScheduler" },
        { "location", "android.location.ILocationManager" },
        { "media_session", "android.media.session.ISessionManager" },
        { "mount", "android.os.storage.IStorageManager" },
        { "notification", "android.app.INotificationManager" },
        { "package", "android.content.pm.IPackageManager" },
        { "permission", "android.os.IPermissionController" },
        { "permissionmgr", "android.permission.IPermissionManager" },
        { "phone", "com.android.internal.telephony.ITelephony" },
        { "power", "android.os.IPowerManager" },
        { "statusbar", "com.android.internal.statusbar.IStatusBarService" },
        { "storagestats", "android.app.usage.IStorageStatsManager" },
        { "telephony.registry", "com.android.internal.telephony.ITelephonyRegistry" },
        { "uimode", "android.app.IUiModeManager" },
        { "usagestats", "android.app.usage.IUsageStatsManager" },
        { "user", "android.os.IUserManager" },
        { "vibrator_manager", "android.os.IVibratorManagerService" },
        { "wifi", "android.net.wifi.IWifiManager" },
        { "window", "android.view.IWindowManager" },
    };

    /** The binders given so far, one a name. */
    private static final java.util.HashMap<String, IBinder> sGiven = new java.util.HashMap<>();

    /** The binder that stands for the platform's service of that name (the same one each time), or null for any other name. */
    private static IBinder find(String name) {
        if (name == null) return null;
        synchronized (sGiven) {
            IBinder b = sGiven.get(name);
            if (b != null) return b;
            for (String[] s : SERVICES) {
                if (s[0].equals(name)) {
                    b = new Binder(s[1]);
                    sGiven.put(name, b);
                    return b;
                }
            }
        }
        return null;
    }

    public static IBinder getService(String name) { return find(name); }

    public static IBinder getServiceOrThrow(String name) throws ServiceNotFoundException {
        IBinder b = find(name);
        if (b == null) throw new ServiceNotFoundException(name);
        return b;
    }

    public static IBinder checkService(String name) { return find(name); }

    public static IBinder waitForService(String name) { return find(name); }

    public static IBinder waitForDeclaredService(String name) { return find(name); }

    public static boolean isDeclared(String name) { return false; }

    public static String[] getDeclaredInstances(String iface) { return new String[0]; }

    public static void addService(String name, IBinder service) {}

    public static void addService(String name, IBinder service, boolean allowIsolated) {}

    public static void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) {}

    public static String[] listServices() {
        String[] names = new String[SERVICES.length];
        for (int i = 0; i < SERVICES.length; i++) names[i] = SERVICES[i][0];
        return names;
    }
}
