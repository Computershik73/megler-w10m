package dalvik.system;

import java.io.IOException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;

/**
 * Android 10's DelegateLastClassLoader in place of Android 9's (the same DEX descriptor; this DEX comes before
 * core-libart's). A class is looked for in the boot class path, then in this loader's own DEX files, then in the parent;
 * so is a resource, the parent's only when delegateResourceLoading says so (Android 9's always asked the parent).
 * Android 10 added the constructor that says that, and Google Play services 26 makes the class loader of each of its
 * modules with it once SDK_INT is 29 or more: with Android 9's class that was a NoSuchMethodError, and Play Store 53 stopped
 * as it started.
 */
public final class DelegateLastClassLoader extends PathClassLoader {
    private final boolean delegateResourceLoading;

    public DelegateLastClassLoader(String dexPath, ClassLoader parent) {
        this(dexPath, null, parent, true);
    }

    public DelegateLastClassLoader(String dexPath, String librarySearchPath, ClassLoader parent) {
        this(dexPath, librarySearchPath, parent, true);
    }

    public DelegateLastClassLoader(String dexPath, String librarySearchPath, ClassLoader parent, boolean delegateResourceLoading) {
        super(dexPath, librarySearchPath, parent);
        this.delegateResourceLoading = delegateResourceLoading;
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        Class<?> cl = findLoadedClass(name);
        if (cl != null) return cl;
        try {
            return Object.class.getClassLoader().loadClass(name);
        } catch (ClassNotFoundException ignored) {
        }
        ClassNotFoundException own;
        try {
            return findClass(name);
        } catch (ClassNotFoundException e) {
            own = e;
        }
        try {
            return getParent().loadClass(name);
        } catch (ClassNotFoundException e) {
            throw own;
        }
    }

    @Override
    public URL getResource(String name) {
        URL r = Object.class.getClassLoader().getResource(name);
        if (r != null) return r;
        r = findResource(name);
        if (r != null || !delegateResourceLoading) return r;
        ClassLoader parent = getParent();
        return parent == null ? null : parent.getResource(name);
    }

    @Override
    public Enumeration<URL> getResources(String name) throws IOException {
        ArrayList<URL> all = new ArrayList<>();
        add(all, Object.class.getClassLoader().getResources(name));
        add(all, findResources(name));
        ClassLoader parent = getParent();
        if (delegateResourceLoading && parent != null) add(all, parent.getResources(name));
        return Collections.enumeration(all);
    }

    private static void add(ArrayList<URL> to, Enumeration<URL> from) {
        while (from != null && from.hasMoreElements()) to.add(from.nextElement());
    }
}
