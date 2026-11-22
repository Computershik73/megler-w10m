// Megler: the class loaders and contexts of an app's isolated splits (android:isolatedSplits), as Android's LoadedApk has them.
package megler;

import android.content.Context;
import android.content.pm.PackageManager;
import java.util.HashMap;

/**
 * An app's feature splits as Android loads them for an app whose manifest says android:isolatedSplits (LoadedApk's
 * SplitDependencyLoaderImpl): the base's class loader has the base alone (AppRunner.installClassLoader), and each feature
 * split is loaded by a PathClassLoader of its own over its APK, made when first asked for, whose parent is the loader of the
 * split it uses (<uses-split>) or the base's. A split's context (Context.createContextForSplit) has that loader, and so has a
 * component the split declares (ComponentInfo.splitName): it is made by it and gets the split's context as its base
 * (ActivityThread.performLaunchActivity, handleCreateService, handleReceiver, installProvider). The runtime gives a split's
 * classes a scope of their own, its parent's first (mg_dvm_isolated_split, mg_java.c).
 *
 * Chrome 158 has its browser in its split "chrome": its Application makes the split's context and gives the application
 * context the split's loader (Chromium's BundleUtils.replaceClassLoader), and its activities, of that split, check that their
 * class's loader is the application context's. With every split on the one loader, and createContextForSplit answering the
 * context itself, Chromium made a loader of its own over the split, whose classes were still the base loader's, and Chrome
 * ended at its start with "ClassLoader mismatch detected." (the HP Elite x3 on 0.1.0.754).
 *
 * For any other app every split is on its one loader, as before (Android: "All Splits are always loaded"), and these give the
 * app's loader and the context itself.
 */
public final class Splits {
    private static final HashMap<String, ClassLoader> sLoaders = new HashMap<>();
    private Splits() {}

    /** Whether the app loads its feature splits in isolation (ApplicationInfo.requestsIsolatedSplitLoading). */
    public static boolean isolated() {
        Manifest.read();
        return Manifest.isolatedSplits;
    }

    /** The split a component of the app's manifest is in, by its own manifest, else (an alias) its target's; null for the base. */
    public static String of(Manifest.Component c) {
        if (c == null) return null;
        if (c.split == null && c.parentActivity != null && "activity-alias".equals(c.kind)) {
            Manifest.Component target = Manifest.activity(c.parentActivity);
            if (target != null) return target.split;
        }
        return c.split;
    }

    /** The path the app knows a split by, as mg_java.c mounts it (Native.splitPaths): null for a split the app has not. */
    private static String path(String split) {
        String[] paths = Native.splitPaths();
        if (paths != null) for (String p : paths) if (p.endsWith("/split_" + split + ".apk")) return p;
        return null;
    }

    /**
     * The class loader of a split, as LoadedApk.getSplitClassLoader gives it: the base's for null or for an app that does not
     * load its splits in isolation; a feature split's own, made once over its APK with its parent the loader of the split it
     * uses or the base's; for a configuration split, the base's. NameNotFoundException for a split the app has not, as
     * Android's "Split name '...' is not installed".
     */
    public static ClassLoader classLoader(String split) throws PackageManager.NameNotFoundException {
        if (split == null || !isolated()) return AppRunner.classLoader();
        String path = path(split);
        if (path == null) throw new PackageManager.NameNotFoundException("Split name '" + split + "' is not installed");
        if (!Manifest.featureSplits.containsKey(split)) return AppRunner.classLoader();
        synchronized (sLoaders) {
            ClassLoader made = sLoaders.get(split);
            if (made != null) return made;
            String uses = Manifest.featureSplits.get(split);
            ClassLoader parent = uses != null && !uses.equals(split) ? classLoader(uses) : AppRunner.classLoader();
            try {
                // (No folder of native libraries, as Android's ApplicationLoaders makes a split's loader: they are the base's.)
                made = (ClassLoader) Class.forName("dalvik.system.PathClassLoader").getConstructor(String.class, String.class, ClassLoader.class)
                        .newInstance(path, null, parent);
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException("the class loader of the split " + split, e);
            }
            sLoaders.put(split, made);
            android.util.Log.i("Megler", "the split " + split + " is loaded in isolation, by a class loader of its own under "
                    + (uses != null ? "the split " + uses + "'s" : "the base's"));
            return made;
        }
    }

    /** The class loader a component of the app's manifest is made by: its split's (classLoader), never a NameNotFoundException. */
    public static ClassLoader loaderOf(Manifest.Component c) {
        try {
            return classLoader(of(c));
        } catch (PackageManager.NameNotFoundException e) {
            return AppRunner.classLoader();
        }
    }

    /** The base context a component of the app's manifest is given, from `base`: the split's context for one of an isolated split. */
    public static Context contextOf(Context base, Manifest.Component c) {
        String split = of(c);
        if (split == null || !isolated()) return base;
        try {
            return base.createContextForSplit(split);
        } catch (PackageManager.NameNotFoundException e) {
            return base;
        }
    }
}
