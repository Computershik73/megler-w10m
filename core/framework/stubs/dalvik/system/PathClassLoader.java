package dalvik.system;

/** Compile-time shape of Android's PathClassLoader, supplied by the Android root at runtime. */
public class PathClassLoader extends BaseDexClassLoader {
    public PathClassLoader(String dexPath, String librarySearchPath, ClassLoader parent) { super(dexPath, null, librarySearchPath, parent); }
}
