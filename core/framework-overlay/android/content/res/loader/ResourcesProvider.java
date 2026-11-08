// Overlay of core/framework/gen-src/android/content/res/loader/ResourcesProvider.java (tools/build-framework.sh).
// It differs in providing resources: the generated stub's providers held nothing. One made from an APK (loadFromApk, the
// descriptor's file; loadFromSplit, the app's split of that name) is that APK opened beside the app's (megler.Native.addApk)
// and its resources.arsc read as a package of its own (Resources.meglerLoaderTable): a ResourcesLoader that holds it puts
// its resources over those of a Resources it is added to (Resources.addLoaders). A provider of a directory or of a table
// alone is empty, as one of empty() is: those are not done here, and the log says so.
//
// Why. Google Play services' DynamiteLoader makes a module's resources so on Android 11 and later (its ChimeraFileApk):
// PackageManager.getResourcesForApplication("android") with a ResourcesLoader of the module's APK added. The Google Maps
// module's renderer reads its textures from those (BitmapFactory.decodeResource of its 0x7f ids): with nothing there, the
// ids were looked up in the app's own resources, no picture came back, and the map's GL thread ended on a null Bitmap --
// Transit's map stayed empty (0.1.0.671).
package android.content.res.loader;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class ResourcesProvider implements java.lang.AutoCloseable, java.io.Closeable {
    private final String mPath;
    private int mApk = -1;
    private android.content.res.ResTable mTable;
    private boolean mOpened;

    protected ResourcesProvider() { this(null); }
    private ResourcesProvider(String path) { mPath = path; }

    /** The path a descriptor was opened from: its file, else what /proc/self/fd says. */
    private static String pathOf(android.os.ParcelFileDescriptor fd) throws java.io.IOException {
        if (fd == null) throw new NullPointerException("fileDescriptor");
        java.io.File f = fd.meglerFile();
        if (f != null) return f.getPath();
        try {
            Object link = Class.forName("android.system.Os").getMethod("readlink", String.class).invoke(null, "/proc/self/fd/" + fd.getFd());
            if (link instanceof String && ((String) link).startsWith("/")) return (String) link;
        } catch (ReflectiveOperationException e) {
            // (Said below.)
        }
        throw new java.io.IOException("Failed to load APK: descriptor " + fd.getFd() + " names no file here");
    }

    public static ResourcesProvider empty(AssetsProvider p0) { return new ResourcesProvider(); }
    public static ResourcesProvider loadFromApk(android.os.ParcelFileDescriptor p0) throws java.io.IOException { return loadFromApk(p0, null); }
    public static ResourcesProvider loadFromApk(android.os.ParcelFileDescriptor p0, AssetsProvider p1) throws java.io.IOException {
        ResourcesProvider p = new ResourcesProvider(pathOf(p0));
        p.open();
        return p;
    }
    /** A part of a file that is an APK: here only the whole of one, from its start. */
    public static ResourcesProvider loadFromApk(android.os.ParcelFileDescriptor p0, long p1, long p2, AssetsProvider p3) throws java.io.IOException {
        if (p1 != 0) throw new java.io.IOException("Failed to load APK: an APK inside another file (at " + p1 + ") is not read here");
        return loadFromApk(p0, p3);
    }
    public static ResourcesProvider loadFromDirectory(String p0, AssetsProvider p1) throws java.io.IOException {
        android.util.Log.w("ResourcesProvider", "loadFromDirectory(" + p0 + "): resources from a directory are not read here; the provider is empty");
        return new ResourcesProvider();
    }
    /** The app's split of that name (ApplicationInfo.splitNames, splitSourceDirs). */
    public static ResourcesProvider loadFromSplit(android.content.Context p0, String p1) throws java.io.IOException {
        android.content.pm.ApplicationInfo a = p0.getApplicationInfo();
        if (a != null && a.splitNames != null && a.splitSourceDirs != null) {
            for (int i = 0; i < a.splitNames.length && i < a.splitSourceDirs.length; i++) {
                if (a.splitNames[i].equals(p1)) {
                    ResourcesProvider p = new ResourcesProvider(a.splitSourceDirs[i]);
                    p.open();
                    return p;
                }
            }
        }
        throw new IllegalArgumentException("Split '" + p1 + "' not found");
    }
    public static ResourcesProvider loadFromTable(android.os.ParcelFileDescriptor p0, long p1, long p2, AssetsProvider p3) throws java.io.IOException {
        android.util.Log.w("ResourcesProvider", "loadFromTable: a resources.arsc alone is not read here; the provider is empty");
        return new ResourcesProvider();
    }
    public static ResourcesProvider loadFromTable(android.os.ParcelFileDescriptor p0, AssetsProvider p1) throws java.io.IOException { return loadFromTable(p0, 0, -1, p1); }
    public static ResourcesProvider loadOverlay(android.content.om.OverlayInfo p0) throws java.io.IOException { return new ResourcesProvider(); }

    /** The APK opened beside the app's and its table read, once. */
    private synchronized void open() throws java.io.IOException {
        if (mOpened) return;
        mOpened = true;
        if (mPath == null) return;
        mApk = megler.Native.addApk(mPath);
        if (mApk < 0) throw new java.io.IOException("Failed to load APK at path " + mPath + " (it cannot be read, or there is no room for one more: the log says which)");
        mTable = android.content.res.Resources.meglerLoaderTable(mApk);
        android.util.Log.i("ResourcesProvider", "resources of " + mPath + " (APK " + mApk + "): " + android.content.res.Resources.meglerDescribeTable(mTable));
    }

    /** Megler: the table this provider gives (null for an empty one) and the core's number of its APK (-1 for none). */
    public android.content.res.ResTable meglerTable() { return mTable; }
    public int meglerApk() { return mApk; }
    public String meglerPath() { return mPath; }

    /** (The APK stays open beside the app's, as the app's own do, for as long as the process runs: nothing to close.) */
    public void close() {}
    protected void finalize() {}
    public android.content.res.ApkAssets getApkAssets() { return null; }
    @Override public String toString() { return "ResourcesProvider{" + (mPath != null ? mPath : "empty") + "}"; }
}
