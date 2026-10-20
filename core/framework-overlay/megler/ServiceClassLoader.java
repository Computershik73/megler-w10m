package megler;

/** A service package namespace, sharing only platform classes with its client. */
public final class ServiceClassLoader extends ClassLoader {
    private final int scope;
    private final String apk;
    public ServiceClassLoader(String apk) throws java.io.IOException {
        super(null);
        this.apk = apk;
        scope = openApk(apk);
    }
    private native int openApk(String apk) throws java.io.IOException;
    private native Class<?> findClassNative(String name) throws ClassNotFoundException;
    @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        // Native lookup delegates to the shared platform, then this package's DEX files.
        Class<?> type = findClassNative(name);
        int dot = name.lastIndexOf('.');
        if (dot > 0 && type.getClassLoader() == this) {
            String pkg = name.substring(0, dot);
            if (getPackage(pkg) == null) definePackage(pkg, null, null, null, null, null, null, null);
        }
        return type;
    }
    @Override protected String findLibrary(String name) { return apk + "!/lib/armeabi-v7a/" + System.mapLibraryName(name); }
    @Override protected Class<?> findClass(String name) throws ClassNotFoundException { return findClassNative(name); }
}
