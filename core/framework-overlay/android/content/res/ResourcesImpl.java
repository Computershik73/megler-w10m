// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// ResourcesImpl, as Android 7 and later have it: what a Resources object is made of -- its AssetManager, its configuration and
// display metrics and the configuration its resources are chosen for, the app package's table, its resources loaders, and what
// was made of them (drawables, colour state lists, fonts, styles' bags) -- kept apart from the Resources object, which holds one
// (Resources.getImpl) and can be given another (Resources.setImpl). The framework's own Resources kept all of it in itself.
//
// Why. Apps and their libraries make Resources of their own over the implementation of the ones the platform gave them, by
// reflection (the methods are hidden API): getImpl of the activity's Resources, a Resources of their own -- a subclass that
// answers some lookups itself and leaves the rest to the original -- made with the hidden constructor Resources(ClassLoader)
// or the public one, and setImpl with that implementation; the new Resources then work exactly as the original (the same
// assets, configuration, metrics, tables, caches and loaders). Tuya Smart's dynamic strings do it for every activity
// (DynamicResourceContextThemeWrapper.getResources): without getImpl the app ended at its first activity on
// "java.lang.NoSuchMethodException: getImpl []" (0.1.0.673).
//
// And the platform's preloaded drawables, which Android 7 to 9 keep here (sPreloadedDrawables, sPreloadedColorDrawables): apps
// that fill them by reflection take this class's field where the class is there, and Resources's only where it is not
// (android/content/res/Resources.java says what Instagram puts in them).
//
// And the screen's changes (megler.Screen): every implementation there is takes the screen's new configuration and metrics
// (meglerScreenChanged), as Android's ResourcesManager applies a new configuration to every ResourcesImpl -- once for an
// implementation that several Resources share. They had been every Resources object, each told by its updateConfiguration.
package android.content.res;

import android.content.res.loader.ResourcesLoader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.LongSparseArray;
import android.util.TypedValue;
import java.util.HashMap;

/**
 * What Resources are made of (Android's hidden class): the AssetManager, the configuration and metrics, the tables, the loaders
 * and the caches. Resources that share one answer alike. The lookups themselves are Resources' (android.content.res.Resources).
 */
public class ResourcesImpl {
    /**
     * The platform's preloaded drawables, by layout direction (LTR, RTL), and its colour drawables, as Android 7 to 9 keep them:
     * empty, for apps to fill (Resources.loadDrawable looks in them before a drawable's file is read). Not final: apps set them.
     */
    @SuppressWarnings({"unchecked", "rawtypes"})
    static LongSparseArray<Drawable.ConstantState>[] sPreloadedDrawables = new LongSparseArray[] { new LongSparseArray<>(), new LongSparseArray<>() };
    static LongSparseArray<Drawable.ConstantState> sPreloadedColorDrawables = new LongSparseArray<>();

    final AssetManager mAssets;
    /** The platform's resources alone, with no app package (Resources.meglerPlatform). */
    final boolean mPlatformOnly;
    final DisplayMetrics mMetrics = new DisplayMetrics();
    final Configuration mConfiguration = Configuration.meglerDevice();
    private final android.view.DisplayAdjustments mDisplayAdjustments;
    /** The configuration the resources are chosen for, made of mConfiguration and mMetrics (updateCfg). */
    ResTable.Config mCfg;
    /** Another package's table, where the AssetManager is that package's (megler.MicroG, megler.Gsf), its splits' merged into it; null for the app's own. */
    final ResTable mPackageTable;
    final HashMap<Integer, Drawable.ConstantState> mDrawableCache = new HashMap<>();
    final HashMap<Integer, ColorStateList> mColorCache = new HashMap<>();
    final HashMap<Integer, Typeface> mFontCache = new HashMap<>();
    /** Styles' bags by id, made once (Resources.meglerBag). */
    HashMap<Integer, Resources.Bag> mBags;
    static final ResourcesLoader[] NO_LOADERS = new ResourcesLoader[0];
    /**
     * The resources loaders added (Resources.addLoaders), in order; copied on change, read without a lock at every lookup. They
     * are the AssetManager's on Android, and so this implementation's: every Resources that has it has them.
     */
    volatile ResourcesLoader[] mLoaders = NO_LOADERS;
    /** What a loader's providers changing does: what was made of them is let go. */
    final ResourcesLoader.UpdateCallbacks mLoaderChanged = l -> meglerFlush();

    /** As Android's: the implementation of an AssetManager's resources for a configuration and metrics (the device's where null). */
    public ResourcesImpl(AssetManager assets, DisplayMetrics metrics, Configuration config, android.view.DisplayAdjustments displayAdjustments) {
        mAssets = assets;
        mPlatformOnly = assets.meglerPlatformOnly();
        mMetrics.setToDefaults();
        if (metrics != null) mMetrics.setTo(metrics);
        /* a configuration given here overrides the device's only where it says something (Android's updateFrom) */
        if (config != null) mConfiguration.updateFrom(config);
        mDisplayAdjustments = displayAdjustments != null ? displayAdjustments : new android.view.DisplayAdjustments();
        updateCfg();
        Resources.meglerLoadTables();
        mPackageTable = assets.meglerPackageApk() != 0 ? Resources.meglerPackageTable(assets.meglerPackageApk(), assets.meglerPackageSplits()) : null;
        synchronized (sAll) {
            // (Those gone are dropped as the list grows: an app that makes a configuration context's Resources at every
            // getResources, as some apps' locale wrappers do, would have it grow without end.)
            if (sAll.size() >= 32 && (sAll.size() & 31) == 0) sAll.removeIf(r -> r.get() == null);
            sAll.add(new java.lang.ref.WeakReference<>(this));
        }
    }

    public AssetManager getAssets() { return mAssets; }
    public DisplayMetrics getDisplayMetrics() { return mMetrics; }
    public Configuration getConfiguration() { return mConfiguration; }
    public android.view.DisplayAdjustments getDisplayAdjustments() { return mDisplayAdjustments; }
    /** The platform's preloaded drawables of the left-to-right direction (Android's getPreloadedDrawables). */
    public LongSparseArray<Drawable.ConstantState> getPreloadedDrawables() { return sPreloadedDrawables != null && sPreloadedDrawables.length > 0 ? sPreloadedDrawables[0] : null; }

    /** A new configuration and metrics (null: unchanged); what was made for the old ones is let go. */
    public void updateConfiguration(Configuration config, DisplayMetrics metrics) {
        if (config != null) mConfiguration.updateFrom(config);
        if (metrics != null) mMetrics.setTo(metrics);
        updateCfg();
        mDrawableCache.clear();
        mColorCache.clear();
    }
    private void updateCfg() {
        Configuration c = mConfiguration;
        String lang = c.locale != null ? c.locale.getLanguage() : "en", country = c.locale != null ? c.locale.getCountry() : "US";
        mCfg = new ResTable.Config(lang, country, c.screenWidthDp, c.screenHeightDp, mMetrics.densityDpi,
                                   (c.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES, Resources.PLATFORM_RESOURCES_SDK);
    }
    /** What was made of these resources is let go (their loaders changed). */
    void meglerFlush() {
        synchronized (this) {
            mDrawableCache.clear();
            mColorCache.clear();
            mFontCache.clear();
            mBags = null;
        }
    }
    public void flushLayoutCache() {}
    public void startPreloading() {}
    public void finishPreloading() {}

    // ---- Android's lookups of the implementation, answered by Resources over it (the lookups are Resources')
    private Resources mOwn;
    private Resources own() {
        Resources r = mOwn;
        if (r == null) {
            r = new Resources((ClassLoader) null);
            r.setImpl(this);
            mOwn = r;
        }
        return r;
    }
    public void getValue(int id, TypedValue outValue, boolean resolveRefs) throws Resources.NotFoundException { own().getValue(id, outValue, resolveRefs); }
    public void getValueForDensity(int id, int density, TypedValue outValue, boolean resolveRefs) throws Resources.NotFoundException { own().getValue(id, outValue, resolveRefs); }
    public int getIdentifier(String name, String defType, String defPackage) { return own().getIdentifier(name, defType, defPackage); }
    public String getResourceName(int id) throws Resources.NotFoundException { return own().getResourceName(id); }
    public String getResourcePackageName(int id) throws Resources.NotFoundException { return own().getResourcePackageName(id); }
    public String getResourceTypeName(int id) throws Resources.NotFoundException { return own().getResourceTypeName(id); }
    public String getResourceEntryName(int id) throws Resources.NotFoundException { return own().getResourceEntryName(id); }
    public CharSequence getQuantityText(int id, int quantity) throws Resources.NotFoundException { return own().getQuantityText(id, quantity); }
    public Configuration[] getSizeConfigurations() { return own().getSizeConfigurations(); }

    // ---- Megler: the screen's changes (see the top of the file)
    /** Every implementation made, while it lives. */
    private static final java.util.ArrayList<java.lang.ref.WeakReference<ResourcesImpl>> sAll = new java.util.ArrayList<>();
    /**
     * The screen was resized or turned: every implementation there is takes `screen` (a configuration of the screen's fields
     * alone, the others undefined) and the metrics of the new size. Their other fields -- an app's own locale, put in with
     * updateConfiguration -- stay as they are.
     */
    static void meglerScreenChanged(Configuration screen, DisplayMetrics m) {
        java.util.ArrayList<ResourcesImpl> live = new java.util.ArrayList<>();
        synchronized (sAll) {
            for (java.util.Iterator<java.lang.ref.WeakReference<ResourcesImpl>> i = sAll.iterator(); i.hasNext();) {
                ResourcesImpl r = i.next().get();
                if (r == null) i.remove();
                else live.add(r);
            }
        }
        for (ResourcesImpl r : live) r.updateConfiguration(screen, m);
    }
}
