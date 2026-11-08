// Overlay of core/framework/src/android/content/res/Resources.java (tools/build-framework.sh).
// It differs in these things. The class Bag (a style's items): put and indexOf find a key by hashing. And updateCfg: resources
// are chosen for the API level of the platform's own resources (PLATFORM_RESOURCES_SDK), not for Build.VERSION.SDK_INT.
// And a theme attribute whose value names an attribute that names another, with no end -- a style's item that names
// itself, as Android's TextAppearance does with textColorHighlight -- is not there: resolveAttribute says false, as
// AssetManager2's Theme::GetAttribute gives up after twenty steps, and obtainStyledAttributes leaves out such a value of the
// theme's own. One that the XML or a style gives and the theme does not resolve stays the attribute it names
// (TYPE_ATTRIBUTE), as AttributeResolution leaves it -- BiP reads its colours so from a style in its activity's theme and
// resolves them in the style's own -- and TypedArray's getters give their default for it where Android's throw: GeckoView's
// getSystemColors (Firefox 152) applies android.R.style.TextAppearance to a theme and asks its colours, and Gecko ended on the
// Java exception when getColor threw (MOZ_CRASH(JNI exception)). Everything else is the original's, unchanged, and so are a
// bag's contents and their order.
//
// Why the second. The framework says it is Android 14 (SDK_INT 34); the platform's resources here are Android 9's
// framework-res.apk, because libcore is Android 9's (tools/dvm-android-root.sh). An app's resources qualified -v29 to -v34 are
// written against the platform resources of those versions: Material 3 keeps its dynamic colours in color-v31 and values-v31,
// where they refer to @android:color/system_neutral2_700 and its like by number. Android 9 has no such resources, and has
// private ones of its own under the same numbers: 0x01060064, a system colour in Android 12, is the selector
// control_checkable_material in Android 9, which asks the theme for a colour, which a -v31 theme answers with a colour that
// refers to 0x01060064. Device Info's first layout with a MaterialCardView recursed 6095 frames deep through that circle
// and ended the app. On a phone that really has Android 9, -v31 resources are never chosen; choosing as that phone does is
// the only choice that fits the resources that are here. (The app's Java still sees SDK_INT 34.) An app's font, style or
// colour that has no choice of that level at all (font-v29, values-v31 alone) is taken from the newest up to 34 instead
// (ResTable.find), the app having asked for it because SDK_INT said so; and what an app's resource names of Android 12's
// system colours is those colours (meglerValue, meglerBag), which Android 9 has private resources of its own under.
//
// Why. The original's Bag.put walks the whole bag for the key before adding it, and a style's bag is built by putting the
// parent's items in one by one and then its own; indexOf walks it again for every attribute a view asks of its style. A
// theme has some nine hundred items and half a dozen ancestors, so a theme cost millions of comparisons: Bag.put alone
// was 2.4 of the 6.3 million bytecode instructions of an app's start, and 2.1 of the 5.5 million of its first dialog
// (docs/JAVA.md, "What an AlertDialog cost").
//
// And shared libraries: the resources of an APK added with AssetManager.addAssetPathAsSharedLibrary -- Android's WebView --
// are a table of their own (meglerAddLibrary), whose package is given the next package id from 0x02, as Android's
// AssetManager2 gives a dynamic package; an id of that package is looked up there (table), its files are read out of that
// APK (apkOf), and getIdentifier finds its names when asked for that package. The original knew two tables, the app's and
// the platform's, and every id that was not the platform's went to the app's.
//
// And Android 12's system colours (android.R.color.system_*, 0x0106001d to 0x010600d1: megler.SystemColors), which Android 9's
// resources do not have: Material 3 asks for them by number once SDK_INT is 31 or more, and Android 9 has colours of its own
// under most of those numbers and nothing under some. getColor and getColorStateList give Material You's colours when the
// app's code asks (by Context.getColor, ContextCompat, Compose's colorResource); when a resource's reference is being
// resolved (a theme's attribute, an XML of the platform's: TypedArray, megler.ResInflate), the number means what Android 9's
// resources mean by it, as before. And getValue gives one where Android 9 has nothing under such a number at all: Aurora
// Store's first screen ended on "Resource ID #0x1060091", which is a watch's colour in Android 9.
//
// And the platform's preloaded drawables, sPreloadedDrawables (one LongSparseArray per layout direction) and
// sPreloadedColorDrawables, which Android 7 to 9 keep in ResourcesImpl and look in before a drawable's file is read, by the
// key Android makes of the value: its asset cookie in the high word and its data (a string's index in the pool) in the low.
// Nothing is preloaded here, so they start empty; but apps put arrays of their own in by reflection, taking Resources's
// field where there is no ResourcesImpl, as here. Instagram swaps each for a LongSparseArray that answers its vector icons,
// whose values ("I1801B8") are no file: without the field it said "Could not enable custom drawable support" and ended in
// its main activity's layout with "File I1801B8 not found".
//
// And the accent colour: the platform's accent_device_default_light and _dark and accent_material_light and _dark, which
// Android 9's DeviceDefault and Material themes give as colorAccent (and so colorControlActivated: switches, check boxes,
// progress bars, a dialog's buttons, a field's line), are the phone's accent colour (megler_set_theme, Native.accentColor),
// as an Android phone's maker puts its own accent in those resources; so are AppCompat's defaults of the same names in the
// app's own table, which its themes give as colorAccent when the app names no colour of its own. An app's own colours are
// left as they are. The colour is given wherever one of those resources is met while a reference is followed
// (meglerValue): a theme's attribute, getColor, a colour state list's item.
//
// And the tables (tableOf): a resources.arsc that its APK stores rather than deflates -- as an app built for Android 11 or
// later must -- is read where it lies in the APK, through a buffer over the file (megler.Native.mapApkFile), not copied into
// the heap. Spotify's is 49.8 MB: read, it was a fifth of the heap on a Lumia 650, and the collector, keeping within what the
// phone had free, ran every second or two (package 664).
//
// And the screen's changes (megler.Screen: the shell resized or turned the app's surface): every Resources the app has --
// its own, the system's, those of configuration contexts and of the packages it hosts -- is known (sAll) and takes the
// screen's new orientation, sizes, layout and density, and DisplayMetrics of the new size, as Android's ResourcesManager
// applies a new configuration to all of an app's resources (meglerScreenChanged). And the sizes the app's resources are made
// for (w600dp, h480dp, sw720dp, the screen layout's size and length), which Android's SizeConfigurationBuckets keeps for
// deciding whether a change of size relaunches an activity (meglerSizeBuckets); getSizeConfigurations says them too.
//
// And resources loaders (Android 11's addLoaders, removeLoaders, clearLoaders, getLoaders; android.content.res.loader): the
// resources of a loader's providers -- APKs, each a package of its own (meglerLoaderTable) -- come before these Resources'
// own, the last loader added first and in it the last provider: an id one of them has is its value and its file, and its
// names are found by getIdentifier. Only the Resources a loader is added to have it. And the platform's own resources
// (meglerPlatform: PackageManager.getResourcesForApplication("android")) are a Resources of their own, with no app package:
// an id of 0x7f is not found there unless a loader has it. Google Play services' DynamiteLoader makes a module's resources
// so on Android 11 and later -- the platform's, and a loader of the module's APK -- and the stubs kept nothing, while
// getResourcesForApplication("android") gave the app's own Resources: the Google Maps module's 0x7f ids were looked up in
// the app's table, its textures were not there, and the map's GL thread ended on a null Bitmap (Transit, 0.1.0.671).
package android.content.res;

import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.util.HashMap;

/**
 * The app's resources: its resources.arsc and the platform's (framework-res, package 0x01), looked up for the device's configuration.
 * Files (layouts, drawables, raw) are read out of the APK they belong to. Drawable and colour inflation is in megler.ResInflate.
 */
public class Resources {
    public static final int ID_NULL = 0;
    public static class NotFoundException extends RuntimeException {
        public NotFoundException() {} public NotFoundException(String s) { super(s); } public NotFoundException(String s, Exception e) { super(s, e); }
    }

    // ---- the tables, shared by every Resources of the app
    static ResTable sApp, sFramework;
    static final Object sLock = new Object();
    private static Resources sSystem;

    private final AssetManager mAssets;
    private ResTable mPackageTable;
    /** Megler: the platform's resources alone, with no app package (meglerPlatform; see the top of the file). */
    private final boolean mPlatformOnly;
    private final DisplayMetrics mMetrics = new DisplayMetrics();
    private final Configuration mConfig = Configuration.meglerDevice();
    private ResTable.Config mCfg;
    private final HashMap<Integer, Drawable.ConstantState> mDrawableCache = new HashMap<>();
    /** The platform's preloaded drawables, by layout direction (LTR, RTL), and colour drawables: empty, for apps to fill (see above). Not final: apps set them. */
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static android.util.LongSparseArray<Drawable.ConstantState>[] sPreloadedDrawables =
        new android.util.LongSparseArray[] { new android.util.LongSparseArray<>(), new android.util.LongSparseArray<>() };
    private static android.util.LongSparseArray<Drawable.ConstantState> sPreloadedColorDrawables = new android.util.LongSparseArray<>();
    private final HashMap<Integer, ColorStateList> mColorCache = new HashMap<>();
    private final HashMap<Integer, Typeface> mFontCache = new HashMap<>();
    /** The class loader for the app's classes, as Android's Resources keeps it (apps' own Resources subclasses set it by reflection). */
    private ClassLoader mClassLoader = Resources.class.getClassLoader();
    public ClassLoader getClassLoader() { return mClassLoader; }

    public Resources(AssetManager a) { this(a, null, null); }
    public Resources(AssetManager a, DisplayMetrics m, Configuration c) {
        mAssets = a;
        mPlatformOnly = a.meglerPlatformOnly();
        mMetrics.setToDefaults();
        if (m != null) mMetrics.setTo(m);
        /* a configuration given here overrides the device's only where it says something (Android's updateFrom) */
        if (c != null) mConfig.updateFrom(c);
        updateCfg();
        loadTables();
        if (a.meglerPackageApk() != 0) mPackageTable = packageTable(a.meglerPackageApk());
        synchronized (sAll) {
            // (Those gone are dropped as the list grows: an app that makes a configuration context's Resources at every
            // getResources, as some apps' locale wrappers do, would have it grow without end.)
            if (sAll.size() >= 32 && (sAll.size() & 31) == 0) sAll.removeIf(r -> r.get() == null);
            sAll.add(new java.lang.ref.WeakReference<>(this));
        }
    }

    // ---- Megler: the screen's changes (see the top of the file)
    /** Every Resources made, while it lives. */
    private static final java.util.ArrayList<java.lang.ref.WeakReference<Resources>> sAll = new java.util.ArrayList<>();
    /**
     * The screen was resized or turned: every Resources there is takes `screen` (a configuration of the screen's fields alone,
     * the others undefined) and the metrics of the new size. Their other fields -- an app's own locale, put in with
     * updateConfiguration -- stay as they are.
     */
    public static void meglerScreenChanged(Configuration screen, DisplayMetrics m) {
        java.util.ArrayList<Resources> live = new java.util.ArrayList<>();
        synchronized (sAll) {
            for (java.util.Iterator<java.lang.ref.WeakReference<Resources>> i = sAll.iterator(); i.hasNext();) {
                Resources r = i.next().get();
                if (r == null) i.remove();
                else live.add(r);
            }
        }
        for (Resources r : live) r.updateConfiguration(screen, m);
    }
    private static int[][] sBuckets;
    /**
     * The sizes the app's resources are made for, as Android's SizeConfigurationBuckets keeps them: the widths (w<N>dp), the
     * heights (h<N>dp) and the smallest widths (sw<N>dp) its configurations name, each sorted, and whether any names a screen
     * layout size (small, large...) or length (long, notlong) -- { widths, heights, smallest, { size, long } }.
     */
    public static int[][] meglerSizeBuckets() {
        synchronized (sLock) {
            loadTables();
            if (sBuckets == null) sBuckets = sApp.meglerSizeBuckets();
            return sBuckets;
        }
    }
    /*
     * Another package's table (Google Play services', its framework's: megler.MicroG, megler.Gsf), read once for each of
     * the APKs, and only with the resources of no language, English and the phone's languages. It was read whole for every
     * Resources made of it, and Google Play services 26's is 84 MB, 58 MB of it the strings of 75 languages: in the Java
     * heap of every app it runs in, and of its 2 GB of addresses, which Yandex Maps ran out of (package 489).
     */
    private static final java.util.HashMap<Integer, ResTable> sPackageTables = new java.util.HashMap<>();
    private static ResTable packageTable(int apk) {
        synchronized (sLock) {
            ResTable t = sPackageTables.get(apk);
            if (t == null) {
                byte[] data = megler.Native.readApkResourceTable(apk, tableLanguages());
                t = new ResTable(data == null ? emptyTable() : data);
                sPackageTables.put(apk, t);
            }
            return t;
        }
    }
    /** English and the phone's languages, by the codes resource tables use. */
    private static String[] tableLanguages() {
        java.util.LinkedHashSet<String> l = new java.util.LinkedHashSet<>();
        try {
            android.os.LocaleList list = android.os.LocaleList.getDefault();
            for (int i = 0; i < list.size(); i++) l.add(list.get(i).getLanguage());
        } catch (Throwable t) {}
        l.add(java.util.Locale.getDefault().getLanguage());
        l.add("en");
        l.remove("");
        return l.toArray(new String[0]);
    }
    private void updateCfg() {
        String lang = mConfig.locale != null ? mConfig.locale.getLanguage() : "en", country = mConfig.locale != null ? mConfig.locale.getCountry() : "US";
        mCfg = new ResTable.Config(lang, country, mConfig.screenWidthDp, mConfig.screenHeightDp, mMetrics.densityDpi,
                                   (mConfig.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES, PLATFORM_RESOURCES_SDK);
    }
    /** The API level of the framework-res.apk in the Android root: Android 9's. It must follow that file (see the head of this file). */
    static final int PLATFORM_RESOURCES_SDK = 28;
    private static void loadTables() {
        synchronized (sLock) {
            if (sApp == null) {
                ResTable t = tableOf(0, 0);
                sApp = t != null ? t : new ResTable(emptyTable());
                // a Play install's split APKs (config.xxhdpi, config.en, feature splits) carry their own tables for the same package
                for (int k = 2; k < 12; k++) {
                    try {
                        ResTable st = tableOf(k, 0);
                        if (st != null) sApp.merge(st);
                    } catch (RuntimeException e) { android.util.Log.w("Resources", "split " + k + "'s resources.arsc: " + e); }
                }
            }
            if (sFramework == null) {
                ResTable t = tableOf(1, 0);
                sFramework = t != null ? t : new ResTable(emptyTable());
            }
        }
    }
    /*
     * Megler: the resources.arsc of the core's APK number `apk` (0 the app's, 1 the platform's, 2-11 the app's splits, then
     * shared libraries), or null when it has none: read where it lies in the APK when the APK stores it (megler.Native.mapApkFile),
     * as an app built for Android 11 or later must, else into the heap. Spotify's is 49.8 MB, a fifth of its heap on a Lumia
     * 650 when it was read: memory the phone did not have, and a collection every second or two (package 664).
     */
    private static ResTable tableOf(int apk, int dynamicId) {
        java.nio.ByteBuffer v = megler.Native.mapApkFile(apk, "resources.arsc");
        if (v != null) return new ResTable(v.asReadOnlyBuffer(), dynamicId);
        byte[] d = megler.Native.readApkFile(apk, "resources.arsc");
        return d != null ? new ResTable(d, dynamicId) : null;
    }
    private static byte[] emptyTable() { return new byte[] { 2, 0, 12, 0, 12, 0, 0, 0, 0, 0, 0, 0 }; }
    /** A table with nothing in it: the app package of the platform's own resources (meglerPlatform). */
    private static ResTable sNone;
    private static ResTable none() { synchronized (sLock) { if (sNone == null) sNone = new ResTable(emptyTable()); return sNone; } }
    private ResTable appTable() { return mPackageTable != null ? mPackageTable : mPlatformOnly ? none() : sApp; }
    ResTable table(int id) {
        if (mLoaders.length != 0) {
            android.content.res.loader.ResourcesProvider p = loaderOf(id);
            if (p != null) return p.meglerTable();
        }
        int p = id >>> 24;
        if (p == 0x7f && mPackageTable != null) return mPackageTable;
        if (p == 1) return sFramework;
        ResTable lib = sLibs[p];
        return lib != null ? lib : mPlatformOnly ? none() : sApp;
    }

    // ---- Megler: resources loaders (see the top of the file)
    private static final android.content.res.loader.ResourcesLoader[] NO_LOADERS = new android.content.res.loader.ResourcesLoader[0];
    /** The loaders added, in order (copied on change: read without a lock at every lookup). */
    private volatile android.content.res.loader.ResourcesLoader[] mLoaders = NO_LOADERS;
    /** What a loader's providers changing does: what was cached of them is let go. */
    private final android.content.res.loader.ResourcesLoader.UpdateCallbacks mLoaderChanged = l -> meglerFlush();
    /** The provider of a loader whose table has this id, the last loader and provider added first; null when none has. */
    private android.content.res.loader.ResourcesProvider loaderOf(int id) {
        android.content.res.loader.ResourcesLoader[] loaders = mLoaders;
        for (int i = loaders.length - 1; i >= 0; i--) {
            android.content.res.loader.ResourcesProvider[] ps = loaders[i].meglerProviders();
            for (int k = ps.length - 1; k >= 0; k--) {
                ResTable t = ps[k].meglerTable();
                if (t != null && t.find(id, mCfg) != null) return ps[k];
            }
        }
        return null;
    }
    /** The id of a resource of a loader's by its name, in the package named (null: any), the last added first; 0 for none. */
    private int loaderIdentifier(String pkg, String type, String name) {
        android.content.res.loader.ResourcesLoader[] loaders = mLoaders;
        for (int i = loaders.length - 1; i >= 0; i--) {
            android.content.res.loader.ResourcesProvider[] ps = loaders[i].meglerProviders();
            for (int k = ps.length - 1; k >= 0; k--) {
                ResTable t = ps[k].meglerTable();
                if (t == null || t.packages.isEmpty()) continue;
                if (pkg != null && !pkg.equals(t.packages.get(0).name)) continue;
                int id = t.identifier(null, type, name);
                if (id != 0) return id;
            }
        }
        return 0;
    }
    /** What these Resources made of resources is let go (their loaders changed). */
    private void meglerFlush() {
        synchronized (this) {
            mDrawableCache.clear();
            mColorCache.clear();
            mFontCache.clear();
            mBags = null;
        }
    }
    /**
     * Adds loaders whose resources come before these Resources' own, the last added first; one already there stays where it
     * is. As Android's, only these Resources have them.
     */
    public void addLoaders(android.content.res.loader.ResourcesLoader... loaders) {
        synchronized (this) {
            java.util.ArrayList<android.content.res.loader.ResourcesLoader> now = new java.util.ArrayList<>(java.util.Arrays.asList(mLoaders));
            for (android.content.res.loader.ResourcesLoader l : loaders) {
                if (l == null || now.contains(l)) continue;
                now.add(l);
                l.registerOnProvidersChangedCallback(this, mLoaderChanged);
            }
            mLoaders = now.toArray(NO_LOADERS);
        }
        meglerFlush();
    }
    public void removeLoaders(android.content.res.loader.ResourcesLoader... loaders) {
        synchronized (this) {
            java.util.ArrayList<android.content.res.loader.ResourcesLoader> now = new java.util.ArrayList<>(java.util.Arrays.asList(mLoaders));
            for (android.content.res.loader.ResourcesLoader l : loaders) {
                if (l != null && now.remove(l)) l.unregisterOnProvidersChangedCallback(this);
            }
            mLoaders = now.toArray(NO_LOADERS);
        }
        meglerFlush();
    }
    public void clearLoaders() {
        synchronized (this) {
            for (android.content.res.loader.ResourcesLoader l : mLoaders) l.unregisterOnProvidersChangedCallback(this);
            mLoaders = NO_LOADERS;
        }
        meglerFlush();
    }
    public java.util.List getLoaders() { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(java.util.Arrays.asList(mLoaders))); }
    /** The table of an APK a ResourcesProvider has opened (megler.Native.addApk): its package as it is, 0x7f, read once. */
    public static ResTable meglerLoaderTable(int apk) { return packageTable(apk); }
    /** For the log: a table's package and how many types it has. */
    public static String meglerDescribeTable(ResTable t) {
        if (t == null || t.packages.isEmpty()) return "no resources";
        ResTable.Package p = t.packages.get(0);
        return "package " + p.name + " (0x" + Integer.toHexString(p.id) + ")";
    }
    /**
     * The platform's own resources, a Resources of their own of the app's display and configuration, with no app package:
     * PackageManager.getResourcesForApplication("android"), as Android's gives a Resources of framework-res.apk alone. A
     * loader added to them puts its package where the app's would be (Google Play services' modules: see the top).
     */
    public static Resources meglerPlatform() {
        Resources app = megler.ContextImpl.app() != null ? megler.ContextImpl.app().getResources() : getSystem();
        return new Resources(AssetManager.meglerForPlatform(), app.getDisplayMetrics(), app.getConfiguration());
    }

    // ---- Megler: shared libraries (see the top of the file)
    /** The tables of shared libraries by the package id each was given, and the core's number of each one's APK. */
    private static final ResTable[] sLibs = new ResTable[256];
    private static final int[] sLibApks = new int[256];
    private static int sNextLibraryId = 0x02;
    /**
     * Reads the resources.arsc of the APK the core has as number `apk` (megler.Native.addApk) as a shared library: its package
     * (id 0 in the table, a dynamic one) is given the next free package id from 0x02 on, which is put in id[0], and its name in
     * name[0]. id[0] stays 0 when the APK has no resources. AssetManager.addAssetPathAsSharedLibrary calls this.
     */
    static void meglerAddLibrary(int apk, int[] id, String[] name) {
        synchronized (sLock) {
            loadTables();
            if (sNextLibraryId >= 0x7f) return;
            ResTable t = tableOf(apk, sNextLibraryId);
            if (t == null || t.packages.isEmpty()) return;
            sLibs[sNextLibraryId] = t;
            sLibApks[sNextLibraryId] = apk;
            id[0] = sNextLibraryId++;
            name[0] = t.packages.get(0).name;
        }
    }
    /** The core's number of the APK that holds the files of a resource id: the platform's (1), a library's, or the app's (0). */
    private int apkOf(int id) {
        if (mLoaders.length != 0) {
            android.content.res.loader.ResourcesProvider lp = loaderOf(id);
            if (lp != null) return lp.meglerApk();
        }
        int p = id >>> 24;
        if (p == 0x7f && mPackageTable != null) return mAssets.meglerPackageApk();
        return p == 1 ? 1 : sLibs[p] != null ? sLibApks[p] : 0;
    }
    /** The table of the shared library of that package name, or null. */
    private static ResTable library(String packageName) {
        if (packageName == null) return null;
        synchronized (sLock) {
            for (ResTable t : sLibs) if (t != null && !t.packages.isEmpty() && packageName.equals(t.packages.get(0).name)) return t;
        }
        return null;
    }

    public static Resources getSystem() { synchronized (sLock) { if (sSystem == null) sSystem = new Resources(new AssetManager()); return sSystem; } }
    public final AssetManager getAssets() { return mAssets; }
    public DisplayMetrics getDisplayMetrics() { return mMetrics; }
    public Configuration getConfiguration() { return mConfig; }
    public void updateConfiguration(Configuration c, DisplayMetrics m) {
        if (c != null) mConfig.updateFrom(c);
        if (m != null) mMetrics.setTo(m);
        updateCfg();
        mDrawableCache.clear(); mColorCache.clear();
    }
    public final void flushLayoutCache() {}
    public ResTable.Config meglerConfig() { return mCfg; }

    // ---- raw lookups
    /** The entry's value, references followed (when resolveRefs), into out; false when there is none. */
    public boolean meglerValue(int id, TypedValue out, boolean resolveRefs) {
        for (int depth = 0; depth < 20; depth++) {
            ResTable t = table(id);
            ResTable.Entry e = t.find(id, mCfg);
            if (e == null) return false;
            out.resourceId = id;
            out.changingConfigurations = 0;
            out.assetCookie = (id >>> 24) == 1 ? 1 : 0;
            out.sourceResourceId = id;
            if (e.complex()) { out.type = TypedValue.TYPE_NULL; out.data = 0; out.string = null; out.density = e.density; return true; }
            t.value(e, out);
            out.resourceId = id;
            if (isAccent(id)) { out.type = TypedValue.TYPE_INT_COLOR_ARGB8; out.data = sAccent; out.string = null; return true; }
            if (!resolveRefs || out.type != TypedValue.TYPE_REFERENCE || out.data == 0) return true;
            // (An app's resource that names one of Android 12's system colours means that colour: Android 9 has private
            // resources of its own under those numbers, which no app can name. ResTable.find, NEWER_TYPES.)
            if (t != sFramework && megler.SystemColors.has(out.data)) { systemColor(out.data, out); return true; }
            id = out.data;
        }
        return false;
    }
    /** The accent resources' numbers (see the top), found by name once; none when the phone told no accent colour. */
    private static int[] sAccentIds;
    private static int sAccent;
    private static boolean isAccent(int id) {
        int[] ids = sAccentIds;
        if (ids == null) {
            int accent = megler.Native.accentColor();
            sAccent = accent | 0xFF000000;
            java.util.ArrayList<Integer> found = new java.util.ArrayList<>();
            if (accent != 0) {
                for (String n : new String[] { "accent_device_default_light", "accent_device_default_dark", "accent_material_light", "accent_material_dark" }) {
                    int f = sFramework == null ? 0 : sFramework.identifier("android", "color", n);
                    if (f != 0) found.add(f);
                    int a = sApp == null ? 0 : sApp.identifier(null, "color", n);
                    if (a != 0) found.add(a);
                }
            }
            ids = new int[found.size()];
            for (int i = 0; i < ids.length; i++) ids[i] = found.get(i);
            sAccentIds = ids;
        }
        for (int x : ids) if (x == id) return true;
        return false;
    }
    public void getValue(int id, TypedValue out, boolean resolveRefs) throws NotFoundException {
        if (meglerValue(id, out, resolveRefs)) return;
        if (megler.SystemColors.has(id)) { systemColor(id, out); return; }
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id));
    }
    /** One of Android 12's system colours (megler.SystemColors), as a value. */
    private static void systemColor(int id, TypedValue out) {
        out.type = TypedValue.TYPE_INT_COLOR_ARGB8;
        out.data = megler.SystemColors.color(id);
        out.string = null;
        out.resourceId = id;
        out.assetCookie = 1;
        out.changingConfigurations = 0;
        out.density = 0;
    }
    /** Whether the colour is asked for by the resolution of a resource -- a theme's attribute, a reference in an XML (TypedArray,
     *  megler.ResInflate, a colour state list) -- rather than by the app's code: then a platform number means Android 9's. */
    private static boolean fromResources() {
        StackTraceElement[] stack = new Throwable().getStackTrace();
        for (int i = 1; i < stack.length && i < 8; i++) {
            String c = stack[i].getClassName();
            if (c.equals("android.content.res.Resources")) continue;
            return c.equals("android.content.res.TypedArray") || c.startsWith("megler.Res") || c.equals("android.content.res.ColorStateList");
        }
        return false;
    }
    public void getValueForDensity(int id, int density, TypedValue out, boolean resolveRefs) { getValue(id, out, resolveRefs); }
    public void getValue(String name, TypedValue out, boolean resolveRefs) { int id = getIdentifier(name, "string", null); getValue(id, out, resolveRefs); }

    /** A bag (style, array, plurals, attr): its items with parents' first; null when the id is not a bag. */
    public Bag meglerBag(int id) {
        if (id == 0) return null;
        HashMap<Integer, Bag> cache = bagCache();
        Bag cached = cache.get(id);
        if (cached != null) return cached;
        ResTable t = table(id);
        ResTable.Entry e = t.find(id, mCfg);
        if (e == null || !e.complex()) return null;
        Bag b = new Bag();
        int parent = t.bagParent(e);
        if (parent != 0 && parent != id) {
            Bag pb = meglerBag(parent);
            if (pb != null) { for (int i = 0; i < pb.n; i++) b.put(pb.keys[i], pb.types[i], pb.data[i], pb.strings[i], pb.dens[i]); }
        }
        int n = t.bagCount(e);
        TypedValue v = new TypedValue();
        for (int i = 0; i < n; i++) {
            int name = t.bagItem(e, i, v);
            // (An app's style item that names one of Android 12's system colours is that colour, as in meglerValue.)
            if (t != sFramework && v.type == TypedValue.TYPE_REFERENCE && megler.SystemColors.has(v.data)) {
                v.data = megler.SystemColors.color(v.data);
                v.type = TypedValue.TYPE_INT_COLOR_ARGB8;
                v.string = null;
            }
            b.put(name, v.type, v.data, v.string, v.density);
        }
        cache.put(id, b);
        return b;
    }
    private HashMap<Integer, Bag> mBags;
    private HashMap<Integer, Bag> bagCache() { if (mBags == null) mBags = new HashMap<>(); return mBags; }
    /** A bag's items, keyed by attribute (or array index). */
    public static final class Bag {
        public int n; public int[] keys = new int[8], types = new int[8], data = new int[8], dens = new int[8]; public CharSequence[] strings = new CharSequence[8];
        // Megler: where each key is in the arrays (its index + 1; 0 for an empty place), found by hashing, so that put and
        // indexOf do not walk the arrays. See the top of the file.
        private int[] slots = new int[16];
        /** The index of a key, or -(place + 1) of the empty place it would take. */
        private int find(int k) {
            int mask = slots.length - 1, h = ((k * 0x9E3779B1) >>> 12) & mask;
            for (;;) {
                int s = slots[h];
                if (s == 0) return -(h + 1);
                if (keys[s - 1] == k) return s - 1;
                h = (h + 1) & mask;
            }
        }
        void put(int k, int t, int d, CharSequence s, int density) {
            int i = find(k);
            if (i >= 0) { types[i] = t; data[i] = d; strings[i] = s; dens[i] = density; return; }
            if (n == keys.length) { keys = java.util.Arrays.copyOf(keys, n * 2); types = java.util.Arrays.copyOf(types, n * 2); data = java.util.Arrays.copyOf(data, n * 2); strings = java.util.Arrays.copyOf(strings, n * 2); dens = java.util.Arrays.copyOf(dens, n * 2); }
            keys[n] = k; types[n] = t; data[n] = d; strings[n] = s; dens[n] = density; n++;
            if (n * 2 <= slots.length) { slots[-i - 1] = n; return; }
            // more than half full: twice the places, every key put in again
            slots = new int[slots.length * 2];
            for (int j = 0; j < n; j++) slots[-find(keys[j]) - 1] = j + 1;
        }
        public int indexOf(int k) { int i = find(k); return i < 0 ? -1 : i; }
    }

    // ---- names
    public int getIdentifier(String name, String defType, String defPackage) {
        if (name == null) return 0;
        String pkg = defPackage, type = defType, n = name;
        if (n.startsWith("@")) n = n.substring(1);
        int colon = n.indexOf(':');
        if (colon >= 0) { pkg = n.substring(0, colon); n = n.substring(colon + 1); }
        int slash = n.indexOf('/');
        if (slash >= 0) { type = n.substring(0, slash); n = n.substring(slash + 1); }
        if (type == null) return 0;
        try { return Integer.parseInt(n); } catch (NumberFormatException e) {}
        if ("android".equals(pkg)) return sFramework.identifier("android", type, n);
        // Megler: a loader's package first (see the top of the file)
        if (mLoaders.length != 0) {
            int fromLoader = loaderIdentifier(pkg, type, n);
            if (fromLoader != 0) return fromLoader;
        }
        // addAssetPath can also add a same-package resource archive (GMS's locale-filtered resources).
        // Its partial table must not hide styles and attributes still present in the base APK.
        if (pkg == null || pkg.equals(mAssets.meglerPackageName())) {
            int own = appTable().identifier(null, type, n);
            if (own != 0) return own;
        }
        ResTable lib = library(pkg);
        if (lib != null) return lib.identifier(pkg, type, n);
        int id = appTable().identifier(null, type, n);
        if (id == 0 && (pkg == null || "android".equals(pkg))) id = sFramework.identifier("android", type, n);
        return id;
    }
    public String getResourceName(int id) { String t = getResourceTypeName(id), e = getResourceEntryName(id); return getResourcePackageName(id) + ":" + t + "/" + e; }
    public String getResourcePackageName(int id) {
        ResTable t = table(id);
        ResTable.Package p = t.pkg(id);
        if (p == null) throw new NotFoundException("Unable to find resource ID #0x" + Integer.toHexString(id));
        // (A loader's resource is its own package's, as Android names it.)
        return (id >>> 24) == 0x7f && (t == sApp || t == mPackageTable) ? mAssets.meglerPackageName() : p.name;
    }
    public String getResourceTypeName(int id) { String s = table(id).typeName(id); if (s == null) throw new NotFoundException("Unable to find resource ID #0x" + Integer.toHexString(id)); return s; }
    public String getResourceEntryName(int id) { String s = table(id).keyName(id, mCfg); if (s == null) throw new NotFoundException("Unable to find resource ID #0x" + Integer.toHexString(id)); return s; }

    // ---- values
    private TypedValue value(int id) { TypedValue v = new TypedValue(); getValue(id, v, true); return v; }
    public CharSequence getText(int id) {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_STRING) return v.string;
        CharSequence s = v.coerceToString();
        if (s != null) return s;
        throw new NotFoundException("String resource ID #0x" + Integer.toHexString(id));
    }
    public CharSequence getText(int id, CharSequence def) { try { return id != 0 ? getText(id) : def; } catch (NotFoundException e) { return def; } }
    public String getString(int id) { return getText(id).toString(); }
    public String getString(int id, Object... args) { return String.format(mConfig.locale, getString(id), args); }
    public CharSequence getQuantityText(int id, int quantity) {
        Bag b = meglerBag(id);
        if (b == null) return getText(id);
        int key = quantity == 1 ? 0x01000006 : quantity == 0 ? 0x01000005 : 0x01000004;
        int i = b.indexOf(key);
        if (i < 0) i = b.indexOf(0x01000004);
        if (i < 0 && b.n > 0) i = 0;
        if (i < 0) throw new NotFoundException("Plural resource ID #0x" + Integer.toHexString(id));
        return itemText(b, i);
    }
    private CharSequence itemText(Bag b, int i) {
        if (b.types[i] == TypedValue.TYPE_STRING) return b.strings[i];
        if (b.types[i] == TypedValue.TYPE_REFERENCE) return getText(b.data[i]);
        return TypedValue.coerceToString(b.types[i], b.data[i]);
    }
    public String getQuantityString(int id, int quantity) { return getQuantityText(id, quantity).toString(); }
    public String getQuantityString(int id, int quantity, Object... args) { return String.format(mConfig.locale, getQuantityString(id, quantity), args); }
    public CharSequence[] getTextArray(int id) {
        Bag b = meglerBag(id);
        if (b == null) throw new NotFoundException("Text array resource ID #0x" + Integer.toHexString(id));
        CharSequence[] r = new CharSequence[b.n];
        for (int i = 0; i < b.n; i++) r[i] = itemText(b, i);
        return r;
    }
    public String[] getStringArray(int id) { CharSequence[] t = getTextArray(id); String[] r = new String[t.length]; for (int i = 0; i < t.length; i++) r[i] = t[i] == null ? null : t[i].toString(); return r; }
    public int[] getIntArray(int id) {
        Bag b = meglerBag(id);
        if (b == null) throw new NotFoundException("Int array resource ID #0x" + Integer.toHexString(id));
        int[] r = new int[b.n];
        for (int i = 0; i < b.n; i++) { if (b.types[i] == TypedValue.TYPE_REFERENCE) { TypedValue v = value(b.data[i]); r[i] = v.data; } else r[i] = b.data[i]; }
        return r;
    }
    public TypedArray obtainTypedArray(int id) {
        Bag b = meglerBag(id);
        if (b == null) throw new NotFoundException("Array resource ID #0x" + Integer.toHexString(id));
        TypedArray a = new TypedArray(this, null, b.n);
        for (int i = 0; i < b.n; i++) {
            TypedValue v = new TypedValue();
            v.type = b.types[i]; v.data = b.data[i]; v.string = b.strings[i]; v.density = b.dens[i];
            if (v.type == TypedValue.TYPE_REFERENCE && v.data != 0) { int ref = v.data; if (meglerValue(ref, v, true)) { if (v.type == TypedValue.TYPE_NULL) { v.type = TypedValue.TYPE_REFERENCE; v.data = ref; } } }
            a.set(i, v);
        }
        return a;
    }
    public int getInteger(int id) { TypedValue v = value(id); if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data; throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid"); }
    public boolean getBoolean(int id) { TypedValue v = value(id); if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data != 0; throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a boolean"); }
    public float getFloat(int id) { TypedValue v = value(id); if (v.type == TypedValue.TYPE_FLOAT) return v.getFloat(); throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a float"); }
    public float getDimension(int id) { TypedValue v = value(id); if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimension(v.data, mMetrics); throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid"); }
    public int getDimensionPixelOffset(int id) { TypedValue v = value(id); if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelOffset(v.data, mMetrics); throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a dimension"); }
    public int getDimensionPixelSize(int id) { TypedValue v = value(id); if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelSize(v.data, mMetrics); throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a dimension"); }
    public float getFraction(int id, int base, int pbase) { TypedValue v = value(id); if (v.type == TypedValue.TYPE_FRACTION) return TypedValue.complexToFraction(v.data, base, pbase); throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a fraction"); }
    @Deprecated public int getColor(int id) { return getColor(id, null); }
    public int getColor(int id, Theme theme) {
        if (megler.SystemColors.has(id) && !fromResources()) return megler.SystemColors.color(id);
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        if (v.type == TypedValue.TYPE_ATTRIBUTE && theme != null) { TypedValue r = new TypedValue(); if (theme.resolveAttribute(v.data, r, true)) return r.data; }
        if (v.type == TypedValue.TYPE_STRING) return loadColorStateList(v, id, theme).getDefaultColor();
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid");
    }
    @Deprecated public ColorStateList getColorStateList(int id) { return getColorStateList(id, null); }
    public ColorStateList getColorStateList(int id, Theme theme) {
        if (megler.SystemColors.has(id) && !fromResources()) return ColorStateList.valueOf(megler.SystemColors.color(id));
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT) return ColorStateList.valueOf(v.data);
        if (v.type == TypedValue.TYPE_ATTRIBUTE && theme != null) { TypedValue r = new TypedValue(); if (theme.resolveAttribute(v.data, r, true)) return r.type == TypedValue.TYPE_STRING ? loadColorStateList(r, r.resourceId, theme) : ColorStateList.valueOf(r.data); }
        return loadColorStateList(v, id, theme);
    }
    /** A colour value: an int, or a colour XML file (a selector). */
    public ColorStateList loadColorStateList(TypedValue v, int id, Theme theme) {
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return ColorStateList.valueOf(v.data);
        if (v.type != TypedValue.TYPE_STRING || v.string == null) throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a color");
        ColorStateList c = theme == null ? mColorCache.get(id) : null;
        if (c != null) return c;
        String file = v.string.toString();
        try {
            XmlResourceParser p = meglerXml(file, (v.resourceId != 0 ? v.resourceId : id));
            c = megler.ResInflate.colorStateList(this, p, theme);
        } catch (Exception e) {
            throw new NotFoundException("File " + file + " from color state list resource ID #0x" + Integer.toHexString(id), e);
        }
        if (theme == null && id != 0) mColorCache.put(id, c);
        return c;
    }

    // ---- drawables
    @Deprecated public Drawable getDrawable(int id) { return getDrawable(id, null); }
    public Drawable getDrawable(int id, Theme theme) { return getDrawableForDensity(id, 0, theme); }
    @Deprecated public Drawable getDrawableForDensity(int id, int density) { return getDrawableForDensity(id, density, null); }
    public Drawable getDrawableForDensity(int id, int density, Theme theme) {
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        return loadDrawable(v, id, theme);
    }
    /** A drawable value: a colour, an image file, or a drawable XML file. */
    public Drawable loadDrawable(TypedValue v, int id, Theme theme) {
        if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT) {
            Drawable p = meglerPreloaded(sPreloadedColorDrawables, v.data, theme);
            return p != null ? p : new android.graphics.drawable.ColorDrawable(v.data);
        }
        if (v.type == TypedValue.TYPE_ATTRIBUTE && theme != null) { TypedValue r = new TypedValue(); if (theme.resolveAttribute(v.data, r, true)) return loadDrawable(r, r.resourceId, theme); }
        if (v.type != TypedValue.TYPE_STRING || v.string == null) {
            if (v.type == TypedValue.TYPE_NULL) return null;
            throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid");
        }
        int rid = v.resourceId != 0 ? v.resourceId : id;
        Drawable.ConstantState cs = mDrawableCache.get(rid);
        if (cs != null) return cs.newDrawable(this, theme);
        android.util.LongSparseArray<Drawable.ConstantState>[] preloaded = sPreloadedDrawables;
        int dir = mConfig.getLayoutDirection();
        if (preloaded != null && dir >= 0 && dir < preloaded.length) {
            Drawable p = meglerPreloaded(preloaded[dir], (((long) v.assetCookie) << 32) | v.data, theme);
            if (p != null) return p;
        }
        String file = v.string.toString();
        Drawable d;
        try {
            if (file.endsWith(".xml")) d = megler.ResInflate.drawable(this, meglerXml(file, rid), theme);
            else d = megler.ResInflate.image(this, meglerFile(file, rid), file, v.density);
        } catch (NotFoundException e) { throw e; }
        catch (Exception e) {
            NotFoundException n = new NotFoundException("File " + file + " from drawable resource ID #0x" + Integer.toHexString(id));
            n.initCause(e);
            throw n;
        }
        if (d != null && !file.endsWith(".xml")) { Drawable.ConstantState s = d.getConstantState(); if (s != null) mDrawableCache.put(rid, s); }
        return d;
    }
    /** A preloaded drawable of that key, made for these resources and given the theme, as ResourcesImpl.loadDrawable does; null when there is none. */
    private Drawable meglerPreloaded(android.util.LongSparseArray<Drawable.ConstantState> preloaded, long key, Theme theme) {
        Object o = preloaded != null ? preloaded.get(key) : null;
        if (!(o instanceof Drawable.ConstantState)) return null;
        Drawable d = ((Drawable.ConstantState) o).newDrawable(this);
        if (d != null && theme != null && d.canApplyTheme()) {
            d = d.mutate();
            d.applyTheme(theme);
        }
        return d;
    }
    public Drawable meglerInflateDrawable(org.xmlpull.v1.XmlPullParser p, Theme t) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        return megler.ResInflate.drawable(this, p, t);
    }
    public android.graphics.Movie getMovie(int id) { return null; }

    // ---- files
    /** The bytes of a file in the APK that holds resource id (res/...). */
    public byte[] meglerFile(String path, int id) {
        byte[] d = megler.Native.readApkFile(apkOf(id), path);
        if (d == null && apkOf(id) == 0) {
            // a split APK may hold it
            for (int k = 2; k < 12 && d == null; k++) d = megler.Native.readApkFile(k, path);
        }
        if (d == null) throw new NotFoundException("File " + path + " not found (resource ID #0x" + Integer.toHexString(id) + ")");
        return d;
    }
    public XmlResourceParser meglerXml(String path, int id) {
        try {
            XmlBlock.Parser p = new XmlBlock(meglerFile(path, id), path).newParser();
            p.mRes = this;
            return p;
        } catch (org.xmlpull.v1.XmlPullParserException e) { throw new NotFoundException(e.getMessage()); }
    }
    public XmlResourceParser getXml(int id) { return getLayoutOrXml(id, "xml"); }
    public XmlResourceParser getLayout(int id) { return getLayoutOrXml(id, "layout"); }
    public XmlResourceParser getAnimation(int id) { return getLayoutOrXml(id, "anim"); }
    private XmlResourceParser getLayoutOrXml(int id, String type) {
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        if (v.type != TypedValue.TYPE_STRING) throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid");
        return meglerXml(v.string.toString(), v.resourceId != 0 ? v.resourceId : id);
    }
    public InputStream openRawResource(int id) { TypedValue v = new TypedValue(); return openRawResource(id, v); }
    public InputStream openRawResource(int id, TypedValue v) {
        getValue(id, v, true);
        if (v.type != TypedValue.TYPE_STRING) throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a file");
        return new ByteArrayInputStream(meglerFile(v.string.toString(), v.resourceId != 0 ? v.resourceId : id));
    }
    public AssetFileDescriptor openRawResourceFd(int id) {
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        String path = v.string.toString();
        long fdoff = megler.Native.apkFileFd(apkOf(id), path);
        if (fdoff == -1) throw new NotFoundException("File " + path + " is compressed and cannot be opened as a file descriptor");
        java.io.FileDescriptor fd = new java.io.FileDescriptor();
        try { java.lang.reflect.Field f = java.io.FileDescriptor.class.getDeclaredField("descriptor"); f.setAccessible(true); f.setInt(fd, (int) (fdoff >>> 40)); } catch (Exception e) {}
        return new AssetFileDescriptor(path, fd, fdoff & 0xFFFFFFFFFFL, megler.Native.apkFileLength(apkOf(id), path));
    }
    /** The density a drawable resource's image was made for. */
    public int meglerDrawableDensity(int id) { TypedValue v = new TypedValue(); return meglerValue(id, v, true) ? (v.density == 0 ? 160 : v.density == 0xffff ? mMetrics.densityDpi : v.density) : 160; }
    public Typeface getFont(int id) {
        Typeface t = mFontCache.get(id);
        if (t != null) return t;
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        String file = v.string == null ? "" : v.string.toString();
        if (file.endsWith(".xml")) t = megler.ResInflate.fontFamily(this, meglerXml(file, id));
        else t = Typeface.meglerFromData(meglerFile(file, id));
        mFontCache.put(id, t);
        return t;
    }

    // ---- animations (megler.ResInflate)
    public android.view.animation.Animation meglerLoadAnimation(int id) { return megler.ResInflate.animation(this, getAnimation(id)); }
    public android.view.animation.Interpolator meglerLoadInterpolator(int id) { return megler.ResInflate.interpolator(this, getAnimation(id)); }
    public android.animation.Animator meglerLoadAnimator(int id) { return megler.ResInflate.animator(this, getAnimation(id)); }

    // ---- themes and attributes
    public final Theme newTheme() { return new Theme(); }
    public TypedArray obtainAttributes(android.util.AttributeSet set, int[] attrs) { return new Theme().obtainStyledAttributes(set, attrs, 0, 0, false); }
    public int getConfigurationChangesMegler() { return 0; }

    /** A theme: attribute values from the styles applied to it, each later style overriding (or not, without force). */
    public final class Theme {
        final HashMap<Integer, TypedValue> mAttrs = new HashMap<>();
        private int mKey;
        private final java.util.ArrayList<int[]> mApplied = new java.util.ArrayList<>();
        Theme() {}
        public Resources getResources() { return Resources.this; }
        public void applyStyle(int resId, boolean force) {
            if (resId == 0) return;
            Bag b = meglerBag(resId);
            if (b == null) return;
            mApplied.add(new int[] { resId, force ? 1 : 0 });
            for (int i = 0; i < b.n; i++) {
                if (!force && mAttrs.containsKey(b.keys[i])) continue;
                TypedValue v = new TypedValue();
                v.type = b.types[i]; v.data = b.data[i]; v.string = b.strings[i]; v.density = b.dens[i];
                mAttrs.put(b.keys[i], v);
            }
            mKey = mKey * 31 + resId;
        }
        public void setTo(Theme o) { mAttrs.clear(); mAttrs.putAll(o.mAttrs); mApplied.clear(); mApplied.addAll(o.mApplied); mKey = o.mKey; }
        public void rebase() {}
        @Override public String toString() {
            String probe = System.getenv("TL_RES_PROBE");
            if (probe != null) for (String x : probe.split(",")) { int id = (int) Long.parseLong(x.replace("0x", ""), 16); ResTable t = table(id); ResTable.Entry e = t.find(id, mCfg); android.util.Log.d("ResProbe", x + ": " + t.meglerDescribe(id) + " table " + (t == sFramework ? "framework" : "app") + " entry " + (e == null ? "null" : "complex=" + e.complex() + " parent=0x" + Integer.toHexString(e.complex() ? t.bagParent(e) : 0) + " count=" + (e.complex() ? t.bagCount(e) : 0)) + " bag " + (meglerBag(id) == null ? "null" : meglerBag(id).n)); } StringBuilder b = new StringBuilder("Theme{"); for (int[] a : mApplied) { Bag g = meglerBag(a[0]); b.append("0x").append(Integer.toHexString(a[0])).append('(').append(g == null ? "missing" : g.n + " attrs").append(") "); } return b.append(mAttrs.size()).append(" attrs}").toString(); }
        public int getChangingConfigurations() { return 0; }
        public int[] getAttributeResolutionStack(int defStyleAttr, int defStyleRes, int explicitStyleRes) { return new int[0]; }
        public int getExplicitStyle(android.util.AttributeSet set) { return set == null ? 0 : set.getStyleAttribute(); }
        public void dump(int priority, String tag, String prefix) {}
        public Drawable getDrawable(int id) { return Resources.this.getDrawable(id, this); }
        public int meglerKey() { return mKey; }
        /** The attribute's value in this theme, attributes followed, and references when resolveRefs. */
        public boolean resolveAttribute(int attr, TypedValue out, boolean resolveRefs) {
            TypedValue v = mAttrs.get(attr);
            for (int depth = 0; v != null && v.type == TypedValue.TYPE_ATTRIBUTE && depth < 20; depth++) v = mAttrs.get(v.data);
            if (v == null || v.type == TypedValue.TYPE_ATTRIBUTE) return false;    /* (a chain with no end: unresolved, as Android has it) */
            out.setTo(v);
            out.resourceId = 0;
            if (resolveRefs && out.type == TypedValue.TYPE_REFERENCE && out.data != 0) {
                int ref = out.data;
                if (meglerValue(ref, out, true)) {
                    if (out.type == TypedValue.TYPE_NULL) { out.type = TypedValue.TYPE_REFERENCE; out.data = ref; out.resourceId = ref; }
                } else { out.type = TypedValue.TYPE_REFERENCE; out.data = ref; out.resourceId = ref; }
            } else if (out.type == TypedValue.TYPE_REFERENCE) out.resourceId = out.data;
            return true;
        }
        public TypedArray obtainStyledAttributes(int[] attrs) { return obtainStyledAttributes(null, attrs, 0, 0); }
        public TypedArray obtainStyledAttributes(int resId, int[] attrs) throws NotFoundException {
            TypedArray a = obtainStyledAttributes(null, attrs, 0, resId);
            return a;
        }
        public TypedArray obtainStyledAttributes(android.util.AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) { return obtainStyledAttributes(set, attrs, defStyleAttr, defStyleRes, true); }
        /**
         * As AttributeResolution::ApplyStyle: for each attribute, the XML's value, else the style named in the XML, else the default
         * style the theme gives for defStyleAttr, else defStyleRes, else the theme's own value; then attribute references are resolved in
         * this theme and plain references to simple values followed.
         */
        TypedArray obtainStyledAttributes(android.util.AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes, boolean useTheme) {
            TypedArray a = new TypedArray(Resources.this, useTheme ? this : null, attrs.length);
            XmlBlock.Parser xp = set instanceof XmlBlock.Parser ? (XmlBlock.Parser) set : null;
            Bag xmlStyle = null, defStyle = null;
            if (set != null) {
                int st = set.getStyleAttribute();
                if (st != 0 && xp != null && xp.meglerStyleIsAttr()) { TypedValue r = new TypedValue(); st = useTheme && resolveAttribute(st, r, false) && r.type == TypedValue.TYPE_REFERENCE ? r.data : 0; }
                if (st != 0) xmlStyle = meglerBag(st);
            }
            if (useTheme && defStyleAttr != 0) {
                TypedValue v = new TypedValue();
                if (resolveAttribute(defStyleAttr, v, false) && (v.type == TypedValue.TYPE_REFERENCE)) defStyle = meglerBag(v.data);
            }
            if (defStyle == null && defStyleRes != 0) defStyle = meglerBag(defStyleRes);
            // the XML's attributes by resource id
            int[] xmlIdx = null;
            if (set != null) {
                int n = set.getAttributeCount();
                xmlIdx = new int[attrs.length];
                java.util.Arrays.fill(xmlIdx, -1);
                for (int i = 0; i < n; i++) {
                    int rid = set.getAttributeNameResource(i);
                    if (rid == 0) {
                        // a text parser (no compiled ids): match android attributes by name
                        if (xp == null) { String nm = set.getAttributeName(i); int id = nm == null ? 0 : sFramework.identifier("android", "attr", nm); if (id != 0) rid = id; }
                        if (rid == 0) continue;
                    }
                    for (int k = 0; k < attrs.length; k++) if (attrs[k] == rid) { xmlIdx[k] = i; break; }
                }
            }
            TypedValue v = new TypedValue();
            for (int k = 0; k < attrs.length; k++) {
                int attr = attrs[k];
                boolean found = false;
                v.type = TypedValue.TYPE_NULL; v.data = 0; v.string = null; v.resourceId = 0; v.density = 0;
                if (xmlIdx != null && xmlIdx[k] >= 0) {
                    int i = xmlIdx[k];
                    if (xp != null) xp.getAttributeTypedValue(i, v);
                    else { v.type = TypedValue.TYPE_STRING; v.string = set.getAttributeValue(i); }
                    found = true;
                }
                if (!found && xmlStyle != null) { int i = xmlStyle.indexOf(attr); if (i >= 0) { fromBag(xmlStyle, i, v); found = true; } }
                if (!found && defStyle != null) { int i = defStyle.indexOf(attr); if (i >= 0) { fromBag(defStyle, i, v); found = true; } }
                boolean fromTheme = false;
                if (!found && useTheme) { TypedValue tv = mAttrs.get(attr); if (tv != null) { v.setTo(tv); found = true; fromTheme = true; } }
                if (!found) continue;
                // ?attr: from this theme, through attributes that name others up to twenty deep, as Theme::GetAttribute.
                if (v.type == TypedValue.TYPE_ATTRIBUTE) {
                    int named = v.data, at = named;
                    TypedValue tv = null;
                    for (int depth = 0; useTheme && depth <= 20; depth++) {
                        tv = mAttrs.get(at);
                        if (tv == null || tv.type != TypedValue.TYPE_ATTRIBUTE) break;
                        at = tv.data;
                        tv = null;
                    }
                    if (tv != null && (tv.type != TypedValue.TYPE_NULL || tv.data == TypedValue.DATA_NULL_EMPTY)) {
                        v.setTo(tv);
                    } else if (fromTheme) {
                        continue;
                    } else {
                        // One the theme does not give stays the attribute it names, as Android's AttributeResolution leaves it:
                        // there (an index, hasValue), of TYPE_ATTRIBUTE, for the app to resolve in a theme of its own choosing.
                        // BiP reads its theme colours so from a style in its activity's theme, resolves them in the style's,
                        // and stopped at its first activity when they were not there.
                        v.type = TypedValue.TYPE_ATTRIBUTE;
                        v.data = named;
                        v.string = null;
                        v.resourceId = 0;
                        a.set(k, v);
                        continue;
                    }
                }
                // @null: a reference to nothing, set explicitly (it hides whatever a style or the theme says)
                if (v.type == TypedValue.TYPE_REFERENCE && v.data == 0) { v.type = TypedValue.TYPE_NULL; v.data = TypedValue.DATA_NULL_UNDEFINED; }
                if (v.type == TypedValue.TYPE_NULL && v.data != TypedValue.DATA_NULL_EMPTY) continue;
                // @ref: to a simple value; references to files or bags stay references (with their file path when it is a file)
                if (v.type == TypedValue.TYPE_REFERENCE && v.data != 0) {
                    int ref = v.data;
                    TypedValue r = new TypedValue();
                    if (meglerValue(ref, r, true)) {
                        if (r.type == TypedValue.TYPE_NULL) { v.resourceId = ref; }
                        else if (r.type == TypedValue.TYPE_ATTRIBUTE && useTheme) { TypedValue t2 = new TypedValue(); if (resolveAttribute(r.data, t2, true)) { v.setTo(t2); v.resourceId = t2.resourceId != 0 ? t2.resourceId : ref; } }
                        else { v.setTo(r); v.resourceId = r.resourceId != 0 ? r.resourceId : ref; }
                    } else v.resourceId = ref;
                }
                a.set(k, v);
            }
            if (xp != null) a.mXml = xp;
            return a;
        }
        private void fromBag(Bag b, int i, TypedValue v) { v.type = b.types[i]; v.data = b.data[i]; v.string = b.strings[i]; v.density = b.dens[i]; v.resourceId = 0; }
        // ---- generated by tools/compat/fillmembers.py (Theme): the platform's members this class does not write (signatures only)
        public void encode(android.view.ViewHierarchyEncoder p0) {}
        public int[] getAllAttributes() { return null; }
        public java.lang.String[] getTheme() { return null; }
        public android.content.res.TypedArray resolveAttributes(int[] p0, int[] p1) { return null; }
        // ---- end of generated members (Theme)
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static void dumpHistory(java.io.PrintWriter p0, java.lang.String p1) {}
    public static int getAttributeSetSourceResId(android.util.AttributeSet p0) { return 0; }
    public static android.content.res.TypedArray obtainAttributes(android.content.res.Resources p0, android.content.res.Resources.Theme p1, android.util.AttributeSet p2, int[] p3) { return null; }
    public static void preloadResources() {}
    public static void registerResourcePaths(java.lang.String p0, android.content.pm.ApplicationInfo p1) {}
    public static void resetPreloadDrawableStateCache() {}
    public static boolean resourceHasPackage(int p0) { return false; }
    public static int selectDefaultTheme(int p0, int p1) { return 0; }
    public static int selectSystemTheme(int p0, int p1, int p2, int p3, int p4, int p5) { return 0; }
    public int applyConfigChanges(android.content.res.Configuration p0) { return 0; }
    public void dump(java.io.PrintWriter p0, java.lang.String p1) {}
    public void finishPreloading() {}
    public java.lang.String getLastResourceResolution() { return null; }
    public android.util.LongSparseArray getPreloadedDrawables() { return sPreloadedDrawables != null && sPreloadedDrawables.length > 0 ? sPreloadedDrawables[0] : null; }
    public android.content.res.Configuration[] getSizeAndUiModeConfigurations() { return getSizeConfigurations(); }
    /* The sizes the app's resources are made for, one configuration for each (meglerSizeBuckets), as Android's AssetManager lists them. */
    public android.content.res.Configuration[] getSizeConfigurations() {
        int[][] b = meglerSizeBuckets();
        java.util.ArrayList<Configuration> list = new java.util.ArrayList<>();
        for (int w : b[0]) { Configuration c = new Configuration(); c.screenWidthDp = w; list.add(c); }
        for (int h : b[1]) { Configuration c = new Configuration(); c.screenHeightDp = h; list.add(c); }
        for (int sw : b[2]) { Configuration c = new Configuration(); c.smallestScreenWidthDp = sw; list.add(c); }
        return list.toArray(new Configuration[0]);
    }
    public boolean hasOverrideDisplayAdjustments() { return false; }
    public android.content.res.XmlResourceParser loadXmlResourceParser(java.lang.String p0, int p1, int p2, java.lang.String p3, boolean p4) { return null; }
    public void parseBundleExtra(java.lang.String p0, android.util.AttributeSet p1, android.os.Bundle p2) {}
    public void parseBundleExtras(android.content.res.XmlResourceParser p0, android.os.Bundle p1) {}
    public void preloadFonts(int p0) {}
    public void setCallbacks(android.content.res.Resources.UpdateCallbacks p0) {}
    public void setLocaleConfig(android.app.LocaleConfig p0) {}
    public void startPreloading() {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static class AssetManagerUpdateHandler implements android.content.res.Resources.UpdateCallbacks {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AssetManagerUpdateHandler(android.content.res.Resources p0) {}
        public void onLoaderUpdated(android.content.res.loader.ResourcesLoader p0) {}
        public void onLoadersChanged(android.content.res.Resources p0, java.util.List p1) {}
        AssetManagerUpdateHandler() { this((android.content.res.Resources) null); }
    }
    public interface UpdateCallbacks extends android.content.res.loader.ResourcesLoader.UpdateCallbacks {
        void onLoadersChanged(android.content.res.Resources p0, java.util.List p1);
    }
    // ---- end of generated nested classes
}
