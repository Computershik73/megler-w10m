package megler;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/**
 * The platform's hidden service interfaces -- IPackageManager, IActivityManager, INotificationManager -- answered in the app's
 * own process, for an app of the platform's own (Android's Settings, which has Android 9's framework after Megler's and so
 * the interfaces themselves).
 *
 * On Android such an app reaches the system's services through these interfaces (AppGlobals.getPackageManager(),
 * ActivityManager.getService(), INotificationManager.Stub.asInterface(ServiceManager.getService("notification"))). Here the
 * binder the service manager gives for such a name carries a local interface (attach): a java.lang.reflect.Proxy of the
 * platform's interface whose calls go to the object that answers for it here (Packages, Activities, Notifications) -- the
 * method of the same name whose parameters take the call's arguments -- and every other method answers what the binder's
 * empty reply would: false, 0, null, an empty list. A List answered where the interface says ParceledListSlice is made one.
 * The interface's Stub.asInterface finds the local interface and calls it directly, as it does in the service's own
 * process.
 */
public final class PlatformInterfaces {
    private PlatformInterfaces() {}

    /** What answers for the interface of that descriptor here, or null where nothing does (every call answered with nothing). */
    private static Object target(String descriptor) {
        switch (descriptor) {
            case "android.content.pm.IPackageManager": return new Packages();
            case "android.app.IActivityManager": return new Activities();
            case "android.app.INotificationManager": return new Notifications();
            case "com.android.internal.app.IBatteryStats": return new Battery();
            case "com.android.internal.app.procstats.IProcessStats": return new Processes();
            case "android.webkit.IWebViewUpdateService": return new WebViews();
            default: return null;
        }
    }

    /** The binder the service manager gives an app of the platform's own for a service of that descriptor (or name). */
    public static Binder binder(String descriptor) { return new Local(descriptor); }

    /**
     * A service's binder whose local interface is made for whatever interface the platform's code asks it for
     * (queryLocalInterface, which the interface's Stub.asInterface calls with its descriptor): the object that answers for it
     * here, or none, every call then answering as the empty reply would -- but for a list, which the proxy of the platform's
     * own would read as null (Display's overlays were null, and Settings ended there).
     */
    private static final class Local extends Binder {
        private final HashMap<String, IInterface> made = new HashMap<>();
        Local(String descriptor) { super(descriptor); }
        @Override public IInterface queryLocalInterface(String descriptor) {
            if (descriptor == null) return null;
            synchronized (made) {
                if (made.containsKey(descriptor)) return made.get(descriptor);
                IInterface i = null;
                try {
                    Class<?> iface = Class.forName(descriptor);
                    if (iface.isInterface() && IInterface.class.isAssignableFrom(iface)) {
                        ClassLoader loader = iface.getClassLoader() != null ? iface.getClassLoader() : PlatformInterfaces.class.getClassLoader();
                        i = (IInterface) Proxy.newProxyInstance(loader, new Class<?>[] { iface }, new Handler(target(descriptor), this));
                    }
                } catch (Throwable t) {
                    android.util.Log.w("Megler", "platform: no local " + descriptor + ": " + t);
                }
                made.put(descriptor, i);
                return i;
            }
        }
    }

    /** The local interface of the service of that name (AppGlobals.getPackageManager: "package"), or null. */
    public static Object service(String name) {
        IBinder b = android.os.ServiceManager.getService(name);
        if (!(b instanceof Binder)) return null;
        String descriptor = ((Binder) b).getInterfaceDescriptor();
        return descriptor == null ? null : b.queryLocalInterface(descriptor);
    }

    private static final class Handler implements InvocationHandler {
        private final Object target;
        private final IBinder binder;
        private final HashMap<Method, Method> found = new HashMap<>();
        Handler(Object target, IBinder binder) { this.target = target; this.binder = binder; }

        @Override public Object invoke(Object proxy, Method m, Object[] args) throws Throwable {
            String name = m.getName();
            int n = args == null ? 0 : args.length;
            if (n == 0 && name.equals("asBinder")) return binder;
            if (m.getDeclaringClass() == Object.class) {
                if (name.equals("equals")) return proxy == args[0];
                if (name.equals("hashCode")) return System.identityHashCode(proxy);
                return "Megler's local " + binder.getInterfaceDescriptor();
            }
            Method t = null;
            if (target != null) {
                synchronized (found) {
                    if (found.containsKey(m)) t = found.get(m);
                    else { t = answering(m); found.put(m, t); }
                }
            }
            if (t == null) return zero(m.getReturnType());
            try {
                return convert(t.invoke(target, args), m.getReturnType());
            } catch (InvocationTargetException e) {
                Throwable c = e.getCause();
                if (c instanceof RuntimeException || c instanceof Error) throw c;
                return zero(m.getReturnType());
            }
        }

        /** The target's public method of that name whose parameters take the interface method's, or null. */
        private Method answering(Method m) {
            Class<?>[] want = m.getParameterTypes();
            for (Method c : target.getClass().getMethods()) {
                if (!c.getName().equals(m.getName()) || c.getDeclaringClass() == Object.class) continue;
                Class<?>[] have = c.getParameterTypes();
                if (have.length != want.length) continue;
                boolean ok = true;
                for (int i = 0; i < have.length && ok; i++) ok = have[i] == Object.class || have[i].isAssignableFrom(want[i]);
                if (ok) { c.setAccessible(true); return c; }
            }
            return null;
        }
    }

    /** What the interface's method returns of the target's answer: a List made the ParceledListSlice it asks for. */
    private static Object convert(Object r, Class<?> type) {
        if (r == null) return zero(type);
        if (type.isPrimitive() || type.isInstance(r)) return r;
        if (r instanceof List) {
            try { return type.getConstructor(List.class).newInstance(r); } catch (Throwable t) {}
        }
        return r;
    }

    /** What the binder's empty reply would have said: false, 0, null; an empty list or array for a list or an array. */
    private static Object zero(Class<?> type) {
        if (type == boolean.class) return Boolean.FALSE;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == char.class) return (char) 0;
        if (type == List.class) return new ArrayList<>();
        // (An array as the proxy reads one of no elements from the empty reply: a length of 0, not null.)
        if (type.isArray()) return java.lang.reflect.Array.newInstance(type.getComponentType(), 0);
        if (type.getName().equals("android.content.pm.ParceledListSlice")) {
            try { return type.getConstructor(List.class).newInstance(new ArrayList<>()); } catch (Throwable t) { return null; }
        }
        return null;
    }

    private static PackageManager pm() { return ContextImpl.app().getPackageManager(); }

    /** An IPackageDataObserver told that the package's data or cache is gone (onRemoveCompleted), as Android tells it; the
     *  interface is the platform's own, which Megler's framework names only as a type. */
    public static void tellRemoved(Object observer, String pkg, boolean succeeded) {
        if (observer == null) return;
        try {
            observer.getClass().getMethod("onRemoveCompleted", String.class, boolean.class).invoke(observer, pkg, succeeded);
        } catch (Throwable t) {
            android.util.Log.w("Megler", "platform: the observer of " + pkg + "'s storage could not be told: " + t);
        }
    }

    /** IPackageManager: the package manager's own answers (android.content.pm.PackageManager, which knows the phone's apps). */
    public static final class Packages {
        public void checkPackageStartable(String pkg, int user) {}
        public int checkUidPermission(String permission, int uid) {
            String[] pkgs = pm().getPackagesForUid(uid);
            if (pkgs != null) for (String p : pkgs) if (pm().checkPermission(permission, p) == PackageManager.PERMISSION_GRANTED) return PackageManager.PERMISSION_GRANTED;
            return PackageManager.PERMISSION_DENIED;
        }
        public int checkPermission(String permission, String pkg, int user) { return pm().checkPermission(permission, pkg); }
        public ActivityInfo getActivityInfo(ComponentName c, int flags, int user) {
            try { return pm().getActivityInfo(c, flags); } catch (PackageManager.NameNotFoundException e) { return null; }
        }
        public ActivityInfo getReceiverInfo(ComponentName c, int flags, int user) {
            try { return pm().getReceiverInfo(c, flags); } catch (PackageManager.NameNotFoundException e) { return null; }
        }
        public ServiceInfo getServiceInfo(ComponentName c, int flags, int user) {
            try { return pm().getServiceInfo(c, flags); } catch (PackageManager.NameNotFoundException e) { return null; }
        }
        public ApplicationInfo getApplicationInfo(String pkg, int flags, int user) {
            try { return pm().getApplicationInfo(pkg, flags); } catch (PackageManager.NameNotFoundException e) { return null; }
        }
        public PackageInfo getPackageInfo(String pkg, int flags, int user) {
            try { return pm().getPackageInfo(pkg, flags); } catch (PackageManager.NameNotFoundException e) { return null; }
        }
        public List<ApplicationInfo> getInstalledApplications(int flags, int user) { return pm().getInstalledApplications(flags); }
        public List<PackageInfo> getInstalledPackages(int flags, int user) { return pm().getInstalledPackages(flags); }
        /** The packages that ask for any of those permissions (the special app access screens). */
        public List<PackageInfo> getPackagesHoldingPermissions(String[] permissions, int flags, int user) {
            ArrayList<PackageInfo> out = new ArrayList<>();
            for (PackageInfo p : pm().getInstalledPackages(flags | PackageManager.GET_PERMISSIONS)) {
                if (p.requestedPermissions == null) continue;
                boolean any = false;
                for (String r : p.requestedPermissions) for (String w : permissions) any |= w.equals(r);
                if (any) out.add(p);
            }
            return out;
        }
        public String[] getAppOpPermissionPackages(String permission) {
            ArrayList<String> out = new ArrayList<>();
            for (PackageInfo p : getPackagesHoldingPermissions(new String[] { permission }, 0, 0)) out.add(p.packageName);
            return out.toArray(new String[0]);
        }
        public boolean isPackageAvailable(String pkg, int user) { return getPackageInfo(pkg, 0, user) != null; }
        public int installExistingPackageAsUser(String pkg, int user, int flags, int reason) { return isPackageAvailable(pkg, user) ? 1 : -3; }
        public boolean getBlockUninstallForUser(String pkg, int user) { return false; }
        public boolean isPackageSuspendedForUser(String pkg, int user) { return false; }
        public boolean setApplicationHiddenSettingAsUser(String pkg, boolean hidden, int user) { return false; }
        public int getApplicationEnabledSetting(String pkg, int user) { return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT; }
        public int getComponentEnabledSetting(ComponentName c, int user) { return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT; }
        public String[] getPackagesForUid(int uid) { return pm().getPackagesForUid(uid); }
        public String getNameForUid(int uid) { return pm().getNameForUid(uid); }
        public int getPackageUid(String pkg, int flags, int user) {
            try { return pm().getPackageUid(pkg, flags); } catch (PackageManager.NameNotFoundException e) { return -1; }
        }
        public List<android.content.pm.ResolveInfo> queryIntentActivities(Intent i, String type, int flags, int user) { return pm().queryIntentActivities(i, flags); }
        public List<android.content.pm.ResolveInfo> queryIntentServices(Intent i, String type, int flags, int user) { return pm().queryIntentServices(i, flags); }
        public List<android.content.pm.ResolveInfo> queryIntentReceivers(Intent i, String type, int flags, int user) { return pm().queryBroadcastReceivers(i, flags); }
        public android.content.pm.ResolveInfo resolveIntent(Intent i, String type, int flags, int user) { return pm().resolveActivity(i, flags); }
        public boolean hasSystemFeature(String name, int version) { return pm().hasSystemFeature(name, version); }
        public boolean isSafeMode() { return false; }
        public boolean isOnlyCoreApps() { return false; }
        public boolean isUpgrade() { return false; }
        public boolean isFirstBoot() { return false; }
    }

    /** IActivityManager: the phone's one user, running; the activity that started this one unknown. */
    public static final class Activities {
        public android.content.pm.UserInfo getCurrentUser() { return android.content.pm.UserInfo.owner(); }
        public boolean isUserRunning(int user, int flags) { return user == 0; }
        public int getProcessLimit() { return -1; }
        public int getLaunchedFromUid(IBinder token) { return android.os.Process.myUid(); }
        public String getLaunchedFromPackage(IBinder token) { return Native.packageName(); }
        public boolean switchUser(int user) { return user == 0; }
        public long[] getProcessPss(int[] pids) { return new long[pids == null ? 0 : pids.length]; }
    }

    /**
     * IBatteryStats: the battery's statistics as the service gives them, a parcel of Android 9's BatteryStatsImpl -- one made
     * now, with its counters and timers made and nothing counted (Megler counts no use of the battery by app). Without it the
     * platform's BatteryStatsHelper made an empty BatteryStatsImpl, whose timers are made only by the service's constructor
     * or by a parcel, and Android's Settings ended on its null phone timer at an app's battery use.
     */
    public static final class Battery {
        public android.os.ParcelFileDescriptor getStatisticsStream() { return handedOver("batterystats", "the battery's statistics", getStatistics()); }
        public byte[] getStatistics() {
            try {
                Class<?> impl = Class.forName("com.android.internal.os.BatteryStatsImpl");
                Class<?> idle = Class.forName("com.android.internal.os.BatteryStatsImpl$PlatformIdleStateCallback");
                Class<?> users = Class.forName("com.android.internal.os.BatteryStatsImpl$UserInfoProvider");
                Object stats = impl.getConstructor(java.io.File.class, android.os.Handler.class, idle, users)
                        .newInstance(null, new android.os.Handler(android.os.Looper.getMainLooper()), null, null);
                android.os.Parcel p = android.os.Parcel.obtain();
                ((android.os.Parcelable) stats).writeToParcel(p, 0);
                byte[] bytes = p.marshall();
                p.recycle();
                return bytes;
            } catch (Throwable t) {
                android.util.Log.w("Megler", "platform: no battery statistics: " + t);
                return null;
            }
        }
        public long computeBatteryTimeRemaining() { return -1; }
        public long computeChargeTimeRemaining() { return -1; }
    }

    /**
     * IProcessStats ("procstats"): the processes' use of memory over time as the service hands it over, a parcel of Android 9's
     * ProcessStats -- one made now, as the service's own is at the start of a period, with nothing counted yet (Megler keeps
     * no such statistics). The service always gives one: Android's Settings reads the descriptor of getStatsOverTime without
     * looking for null, and Developer options ended there (ProcStatsData.load, for its Memory item).
     */
    public static final class Processes {
        public android.os.ParcelFileDescriptor getStatsOverTime(long minTime) { return handedOver("procstats", "the processes' statistics", getCurrentStats(null)); }
        /** (The periods before this one, which the service adds to the list as files: none.) */
        public byte[] getCurrentStats(List<?> historic) {
            try {
                // (not running: the constructor that is the running service's asks Debug.MemoryInfo for what it does not have here)
                Object stats = Class.forName("com.android.internal.app.procstats.ProcessStats").getConstructor(boolean.class).newInstance(false);
                Class<?> c = stats.getClass();
                long now = android.os.SystemClock.uptimeMillis();
                // As ProcessStatsService.getStatsOverTime: the period ends now.
                c.getField("mTimePeriodEndRealtime").setLong(stats, android.os.SystemClock.elapsedRealtime());
                c.getField("mTimePeriodEndUptime").setLong(stats, now);
                android.os.Parcel p = android.os.Parcel.obtain();
                c.getMethod("writeToParcel", android.os.Parcel.class, long.class, int.class).invoke(stats, p, now, 0);
                byte[] bytes = p.marshall();
                p.recycle();
                return bytes;
            } catch (Throwable t) {
                android.util.Log.w("Megler", "platform: no statistics of the processes: " + (t instanceof InvocationTargetException ? t.getCause() : t));
                return null;
            }
        }
        /** ProcessStats.ADJ_MEM_FACTOR_NORMAL: memory is not short. */
        public int getCurrentMemoryState() { return 0; }
    }

    /**
     * IWebViewUpdateService ("webviewupdate", android.webkit.WebViewFactory.getUpdateService): the WebView of the Android
     * root, the one there is (WebView.getCurrentWebViewPackage), as the current package and the only valid one. Android's
     * Settings' Developer options show it as the WebView implementation.
     */
    public static final class WebViews {
        public PackageInfo getCurrentWebViewPackage() { return android.webkit.WebView.getCurrentWebViewPackage(); }
        public String getCurrentWebViewPackageName() { PackageInfo p = getCurrentWebViewPackage(); return p == null ? null : p.packageName; }
        public Object getValidWebViewPackages() { return providers(); }
        public Object getAllWebViewPackages() { return providers(); }
        /** Android 9's WebViewProviderInfo[] of that one package (the platform's class: Megler's framework has none). */
        private Object providers() {
            PackageInfo p = getCurrentWebViewPackage();
            try {
                Class<?> c = Class.forName("android.webkit.WebViewProviderInfo");
                Object a = java.lang.reflect.Array.newInstance(c, p == null ? 0 : 1);
                if (p != null) {
                    Object info = c.getConstructor(String.class, String.class, boolean.class, boolean.class, String[].class)
                            .newInstance(p.packageName, "Android System WebView", true, false, null);
                    java.lang.reflect.Array.set(a, 0, info);
                }
                return a;
            } catch (Throwable t) {
                return null;
            }
        }
    }

    /** Those bytes as a service hands them over to be read: a descriptor of a file that has them (null where there are none). */
    private static android.os.ParcelFileDescriptor handedOver(String name, String what, byte[] bytes) {
        if (bytes == null) return null;
        try {
            java.io.File f = java.io.File.createTempFile(name, ".parcel", ContextImpl.app().getCacheDir());
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) { out.write(bytes); }
            android.os.ParcelFileDescriptor pfd = android.os.ParcelFileDescriptor.open(f, android.os.ParcelFileDescriptor.MODE_READ_ONLY);
            f.delete();
            return pfd;
        } catch (java.io.IOException e) {
            android.util.Log.w("Megler", "platform: " + what + " could not be handed over: " + e);
            return null;
        }
    }

    /** INotificationManager: every app may post notifications, in its one default channel. */
    public static final class Notifications {
        public boolean areNotificationsEnabledForPackage(String pkg, int uid) { return true; }
        public boolean areNotificationsEnabled(String pkg) { return true; }
        public boolean canShowBadge(String pkg, int uid) { return true; }
        public boolean onlyHasDefaultChannel(String pkg, int uid) { return true; }
        public int getNumNotificationChannelsForPackage(String pkg, int uid, boolean includeDeleted) { return 0; }
        public int getBlockedAppCount(int user) { return 0; }
        public int getBlockedChannelCount(String pkg, int uid) { return 0; }
        public int getDeletedChannelCount(String pkg, int uid) { return 0; }
        public List<Object> getRecentNotifyingAppsForUser(int user) { return new ArrayList<>(); }
        public List<Object> getNotificationChannelGroupsForPackage(String pkg, int uid, boolean includeDeleted) { return new ArrayList<>(); }
    }
}
