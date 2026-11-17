package dalvik.system;

/** Compile-time shape of Android's BaseDexClassLoader, supplied by the Android root at runtime (megler.ServiceClassLoader is one). */
public class BaseDexClassLoader extends ClassLoader {
    public BaseDexClassLoader(String dexPath, java.io.File optimizedDirectory, String librarySearchPath, ClassLoader parent) { super(parent); }
    @Override protected Class<?> findClass(String name) throws ClassNotFoundException { throw new ClassNotFoundException(name); }
    public String findLibrary(String name) { return null; }
}
