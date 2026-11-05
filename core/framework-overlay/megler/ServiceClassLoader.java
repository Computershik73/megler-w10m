package megler;

/** A service package namespace, sharing only platform classes with its client. */
public final class ServiceClassLoader extends ClassLoader {
    private final int scope;
    private final String apk;
    private final String libraries;
    public ServiceClassLoader(String apk) throws java.io.IOException {
        this(apk, null);
    }
    /**
     * A scope over an app's own APK for another of its processes (megler.Processes), whose native libraries are the app's, in its
     * library folder (ApplicationInfo.nativeLibraryDir): the Java driver puts there those of every one of the app's APKs, and a
     * library of a split is not in the base this scope is over. Play Store from a bundle has its libraries in its split for
     * 32-bit ARM, and its background process, which installs apps, could not load libmappedcountercacheversionjni.so: every
     * install ended in error 1000 (package 574).
     */
    public ServiceClassLoader(String apk, String libraries) throws java.io.IOException {
        super(null);
        this.apk = apk;
        this.libraries = libraries;
        scope = openApk(apk);
    }
    private native int openApk(String apk) throws java.io.IOException;
    private native Class<?> findClassNative(String name) throws ClassNotFoundException;
    @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        // Native lookup delegates to the shared platform, then this package's DEX files.
        Class<?> type = findClassNative(name);
        return type;
    }
    // DEX references also resolve classes without going through loadClass. Class.getPackage
    // must still find metadata for these definitions (microG's Conscrypt provider uses it).
    @Override protected synchronized Package getPackage(String name) {
        Package p = super.getPackage(name);
        return p != null ? p : definePackage(name, null, null, null, null, null, null, null);
    }
    @Override protected String findLibrary(String name) {
        String file = System.mapLibraryName(name);
        if (libraries != null && new java.io.File(libraries, file).isFile()) return libraries + "/" + file;
        return apk + "!/lib/" + Abis.PRIMARY + "/" + file;
    }
    @Override protected Class<?> findClass(String name) throws ClassNotFoundException { return findClassNative(name); }
}
