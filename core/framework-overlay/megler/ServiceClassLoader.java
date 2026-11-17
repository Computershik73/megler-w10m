package megler;

/**
 * A service package namespace, sharing only platform classes with its client.
 *
 * It is a PathClassLoader over its APK, as Android's loader of a package's process is: its path list holds the APK's DexFile
 * and its native library folders, and apps read them by reflection. Facebook Lite's shell does in every one of its processes
 * (its ClassLoaderWithDexPromotion takes BaseDexClassLoader.pathList's dexElements, and puts itself in the loader's parent
 * field); in its push process ":fbns", a scope of megler.Processes, that ended in "Expected receiver of type
 * dalvik/system/BaseDexClassLoader, but got megler/ServiceClassLoader" (packages 696, 699). The classes are the scope's as
 * before, all of them there at once (findClassNative): the path list's DexFile opening the APK is what makes the scope
 * (dvm/mg_dvm_art.c, DexFile_openDexFileNative), and openApk makes it where that did not.
 */
public final class ServiceClassLoader extends dalvik.system.PathClassLoader {
    private final int scope;
    private final String apk;
    private final String libraries;
    /* The parent it was made with; another one there is the app's doing (loadClass). */
    private final ClassLoader madeUnder;
    /** A service hosted here (Google Play services, the WebView): no parent, the platform's classes being the scope's own. */
    public ServiceClassLoader(String apk) throws java.io.IOException {
        this(apk, null, null);
    }
    /**
     * A scope over an app's own APK for another of its processes (megler.Processes), whose native libraries are the app's, in its
     * library folder (ApplicationInfo.nativeLibraryDir): the Java driver puts there those of every one of the app's APKs, and a
     * library of a split is not in the base this scope is over. Play Store from a bundle has its libraries in its split for
     * 32-bit ARM, and its background process, which installs apps, could not load libmappedcountercacheversionjni.so: every
     * install ended in error 1000 (package 574). Its parent is the boot class path's loader, as the app's own loader's is
     * (megler.AppRunner): Facebook Lite's loader, once it is the parent, asks the parent this one had for the platform's classes.
     */
    public ServiceClassLoader(String apk, String libraries) throws java.io.IOException {
        this(apk, libraries, ServiceClassLoader.class.getClassLoader());
    }
    private ServiceClassLoader(String apk, String libraries, ClassLoader parent) throws java.io.IOException {
        super(apk, libraryPath(apk, libraries), parent);
        this.apk = apk;
        this.libraries = libraries;
        this.madeUnder = parent;
        scope = openApk(apk);
    }
    /* Where findLibrary looks, for the path list to say the same. */
    private static String libraryPath(String apk, String libraries) {
        String stored = apk + "!/lib/" + Abis.PRIMARY;
        return libraries != null ? libraries + java.io.File.pathSeparator + stored : stored;
    }
    private native int openApk(String apk) throws java.io.IOException;
    private native Class<?> findClassNative(String name) throws ClassNotFoundException;
    @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        ClassNotFoundException none;
        synchronized (this) {
            // Native lookup delegates to the shared platform, then this package's DEX files.
            try { return findClassNative(name); } catch (ClassNotFoundException e) { none = e; }
        }
        // A parent the app has put there itself (Facebook Lite's, which opens another of its DEX files when a class is not
        // found and defines the class through that DexFile, for this loader) is asked for what the scope has not: Android
        // asks it first, and it finds the same classes, a scope having one of each name. Not under the monitor: it has locks
        // of its own.
        ClassLoader above = getParent();
        if (above == null || above == madeUnder) throw none;
        try { return above.loadClass(name); } catch (ClassNotFoundException e) { throw none; }
    }
    // (No getPackage of its own: DEX references also resolve classes without going through loadClass, and Class.getPackage
    // must still find metadata for these definitions, as microG's Conscrypt provider asks; BaseDexClassLoader.getPackage
    // makes a package asked for, as on Android.)
    @Override public String findLibrary(String name) {
        String file = System.mapLibraryName(name);
        if (libraries != null && new java.io.File(libraries, file).isFile()) return libraries + "/" + file;
        return apk + "!/lib/" + Abis.PRIMARY + "/" + file;
    }
    @Override protected Class<?> findClass(String name) throws ClassNotFoundException { return findClassNative(name); }
}
