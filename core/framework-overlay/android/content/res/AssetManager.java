// Overlay of core/framework/src/android/content/res/AssetManager.java (tools/build-framework.sh).
// It differs in shared libraries: addAssetPathAsSharedLibrary, addAssetPath, getAssignedPackageIdentifiers, getApkPaths
// and findCookieForPath answer, where the original's were signatures answering 0 or null; and open, openFd, openNonAsset,
// openNonAssetFd and list look in such a library's APK too, after the app's own.
//
// Why. Android's WebView is an APK of its own whose code runs in the app (android.webkit.WebViewFactory loads it), and its
// resources and assets are added to the app's AssetManager as a shared library: WebViewFactory and
// WebViewDelegate.addWebViewAssetPath call addAssetPathAsSharedLibrary with the WebView APK's path, the WebView's code asks
// getAssignedPackageIdentifiers for the package id its resources were given (WebViewDelegate.getPackageId) and renumbers
// its R class to it, and Chromium maps icudtl.dat, its .pak files and V8's snapshot out of the WebView APK with
// openNonAssetFd on the app's AssetManager (org.chromium.base.ApkAssets). With the original's every one of those failed.
//
// How. A library's APK is opened by the core beside the app's (meglet.Native.addApk: its number among the APKs the core
// reads, as the framework-res's is 1), its resources.arsc is read by Resources.megletAddLibrary, which gives its package the
// next free package id from 0x02 up, as Android's AssetManager2 does, and its files are read through the same calls as the
// app's own. There is one AssetManager's worth of APKs for the process, as there is one app: the list is static. A cookie is
// 1 for the app's APK and 2 on for the libraries, in the order they were added. Everything else is the original's.
package android.content.res;

import java.io.*;

/** The APK's assets/ folder, read through Meglet (the APK is a zip Meglet already has open); and shared libraries' APKs beside it. */
public final class AssetManager implements AutoCloseable {
    public static final int ACCESS_UNKNOWN = 0, ACCESS_RANDOM = 1, ACCESS_STREAMING = 2, ACCESS_BUFFER = 3;
    public AssetManager() {}
    private static String norm(String p) { while (p.startsWith("/")) p = p.substring(1); while (p.startsWith("./")) p = p.substring(2); return p; }
    public InputStream open(String name) throws IOException { return open(name, ACCESS_STREAMING); }
    public InputStream open(String name, int mode) throws IOException {
        String n = norm(name);
        long len = meglet.Native.assetLength(n);
        // a large asset stored uncompressed is read from the APK as it is consumed, not copied whole into memory (a game's
        // repacked data, hundreds of MB, unpacked through this stream on first launch)
        if (len > (8 << 20)) {
            long fdoff = meglet.Native.assetFd(n);
            if (fdoff != -1) return new StoredAssetStream((int) (fdoff >>> 40), fdoff & 0xFFFFFFFFFFL, len);
        }
        byte[] b = meglet.Native.readAsset(n);
        if (b == null) b = libraryFile("assets/" + n);
        if (b == null) throw new FileNotFoundException(name);
        return new ByteArrayInputStream(b);
    }
    public String[] list(String path) throws IOException {
        String[] l = meglet.Native.listAssets(norm(path));
        if (l == null) l = new String[0];
        synchronized (sLibs) {
            if (sLibs.isEmpty()) return l;
        }
        // a library's assets are listed too, each name once
        java.util.LinkedHashSet<String> all = new java.util.LinkedHashSet<>(java.util.Arrays.asList(l));
        String prefix = "assets/" + norm(path);
        if (!prefix.endsWith("/")) prefix += "/";
        for (Library lib : libraries()) all.addAll(lib.list(prefix));
        return all.toArray(new String[0]);
    }
    public AssetFileDescriptor openFd(String name) throws IOException { return fd(norm(name), name); }
    /** A file from the APK's root rather than assets/ (SoLoader reads lib/<abi>/*.so this way) */
    public AssetFileDescriptor openNonAssetFd(String name) throws IOException { return fd("/" + norm(name), name); }
    public AssetFileDescriptor openNonAssetFd(int cookie, String name) throws IOException {
        Library lib = library(cookie);
        if (lib != null) {
            AssetFileDescriptor afd = lib.fd(norm(name));
            if (afd == null) throw new FileNotFoundException("This file can not be opened as a file descriptor; it is probably compressed: " + name);
            return afd;
        }
        return openNonAssetFd(name);
    }
    public InputStream openNonAsset(String name) throws IOException {
        byte[] b = meglet.Native.readAsset("/" + norm(name));
        if (b == null) b = libraryFile(norm(name));
        if (b == null) throw new FileNotFoundException(name);
        return new ByteArrayInputStream(b);
    }
    public InputStream openNonAsset(int cookie, String name) throws IOException {
        Library lib = library(cookie);
        if (lib != null) {
            byte[] b = meglet.Native.readApkFile(lib.apk, norm(name));
            if (b == null) throw new FileNotFoundException(name);
            return new ByteArrayInputStream(b);
        }
        return openNonAsset(name);
    }
    public InputStream openNonAsset(String name, int mode) throws IOException { return openNonAsset(name); }
    public InputStream openNonAsset(int cookie, String name, int mode) throws IOException { return openNonAsset(cookie, name); }
    private AssetFileDescriptor fd(String n, String name) throws IOException {
        long fdoff = meglet.Native.assetFd(n);
        if (fdoff == -1) {
            // not the app's: a library's (n is "x" for assets/x, "/x" for x from the APK's root)
            String inApk = n.startsWith("/") ? n.substring(1) : "assets/" + n;
            for (Library lib : libraries()) {
                AssetFileDescriptor afd = lib.fd(inApk);
                if (afd != null) return afd;
            }
            throw new FileNotFoundException("This file can not be opened as a file descriptor; it is probably compressed: " + name);
        }
        long len = meglet.Native.assetLength(n);
        return new AssetFileDescriptor(n, descriptor((int) (fdoff >>> 40)), fdoff & 0xFFFFFFFFFFL, len);
    }
    private static FileDescriptor descriptor(int number) {
        FileDescriptor fd = new FileDescriptor();
        try { java.lang.reflect.Field f = FileDescriptor.class.getDeclaredField("descriptor"); f.setAccessible(true); f.setInt(fd, number); } catch (Exception e) { }
        return fd;
    }
    public String[] getLocales() { return new String[] { "en-US" }; }

    // ---- Meglet: shared libraries (see the top of the file)

    /** A shared library's APK: where it is, the core's number for it, and the package id its resources were given (0: none). */
    private static final class Library {
        final String path;
        final int apk;
        int packageId;
        String packageName;
        Library(String path, int apk) { this.path = path; this.apk = apk; }
        /** A file of the APK as a descriptor and where it starts, or null when it is not there or is compressed. */
        AssetFileDescriptor fd(String inApk) {
            long fdoff = meglet.Native.apkFileFd(apk, inApk);
            if (fdoff == -1) return null;
            return new AssetFileDescriptor(inApk, descriptor((int) (fdoff >>> 40)), fdoff & 0xFFFFFFFFFFL, meglet.Native.apkFileLength(apk, inApk));
        }
        java.util.List<String> list(String prefix) {
            String[] all = meglet.Native.listApkFiles(apk, prefix);
            return all == null ? java.util.Collections.<String>emptyList() : java.util.Arrays.asList(all);
        }
    }
    private static final java.util.ArrayList<Library> sLibs = new java.util.ArrayList<>();

    private static java.util.List<Library> libraries() { synchronized (sLibs) { return new java.util.ArrayList<>(sLibs); } }
    /** The library of a cookie: 2 is the first library, 1 the app's own APK (null here). */
    private static Library library(int cookie) { synchronized (sLibs) { return cookie >= 2 && cookie - 2 < sLibs.size() ? sLibs.get(cookie - 2) : null; } }
    private static byte[] libraryFile(String inApk) {
        for (Library lib : libraries()) {
            byte[] b = meglet.Native.readApkFile(lib.apk, inApk);
            if (b != null) return b;
        }
        return null;
    }

    /**
     * Adds an APK whose resources are a shared library (package id 0 in its table, given an id here) and whose files join the
     * app's: Android's WebView APK. Its cookie, or 0 when it cannot be read. Adding the same path again gives the same cookie.
     */
    public int addAssetPathAsSharedLibrary(String path) {
        if (path == null) return 0;
        synchronized (sLibs) {
            for (int i = 0; i < sLibs.size(); i++) if (sLibs.get(i).path.equals(path)) return i + 2;
            int apk = meglet.Native.addApk(path);
            if (apk < 0) {
                android.util.Log.w("AssetManager", "cannot add " + path + ": it cannot be read");
                return 0;
            }
            Library lib = new Library(path, apk);
            sLibs.add(lib);
            int[] id = new int[1];
            String[] name = new String[1];
            Resources.megletAddLibrary(apk, id, name);
            lib.packageId = id[0];
            lib.packageName = name[0];
            android.util.Log.i("AssetManager", "added " + path + (lib.packageId != 0 ? ": package " + lib.packageName + " as 0x" + Integer.toHexString(lib.packageId) : ": no resources"));
            return sLibs.size() + 1;
        }
    }
    /** The app's own APK is cookie 1; any other is added as above (the only kind an app adds is a library's). */
    public int addAssetPath(String path) {
        if (path == null) return 0;
        if (path.equals(meglet.Native.apkPath())) return 1;
        return addAssetPathAsSharedLibrary(path);
    }
    public int addOverlayPath(String path) { return 0; }
    /** Package id to package name of every package the resources have: the platform's, the app's, and each library's. */
    public android.util.SparseArray<String> getAssignedPackageIdentifiers() { return getAssignedPackageIdentifiers(true, true); }
    public android.util.SparseArray<String> getAssignedPackageIdentifiers(boolean includeOverlays, boolean includeLoaders) {
        android.util.SparseArray<String> ids = new android.util.SparseArray<>();
        ids.put(0x01, "android");
        ids.put(0x7f, meglet.Native.packageName());
        for (Library lib : libraries()) if (lib.packageId != 0) ids.put(lib.packageId, lib.packageName);
        return ids;
    }
    public String[] getApkPaths() {
        java.util.List<Library> libs = libraries();
        String[] paths = new String[1 + libs.size()];
        paths[0] = meglet.Native.apkPath();
        for (int i = 0; i < libs.size(); i++) paths[i + 1] = libs.get(i).path;
        return paths;
    }
    public int findCookieForPath(String path) {
        if (path == null) return 0;
        if (path.equals(meglet.Native.apkPath())) return 1;
        synchronized (sLibs) { for (int i = 0; i < sLibs.size(); i++) if (sLibs.get(i).path.equals(path)) return i + 2; }
        return 0;
    }

    /** An uncompressed asset read in place from the APK through its own descriptor. */
    private static final class StoredAssetStream extends AssetInputStream {
        private final FileInputStream file;
        private final java.nio.channels.FileChannel ch;
        private final long start, len;
        private long pos, mark;
        StoredAssetStream(int fdNum, long start, long len) {
            file = new FileInputStream(descriptor(fdNum)); ch = file.getChannel();
            this.start = start; this.len = len;
        }
        @Override public int read() throws IOException { byte[] one = new byte[1]; return read(one, 0, 1) == 1 ? one[0] & 0xff : -1; }
        @Override public int read(byte[] b, int off, int n) throws IOException {
            if (n == 0) return 0;
            if (pos >= len) return -1;
            if (n > len - pos) n = (int) (len - pos);
            int r = ch.read(java.nio.ByteBuffer.wrap(b, off, n), start + pos);
            if (r <= 0) return -1;
            pos += r;
            return r;
        }
        @Override public long skip(long n) { if (n <= 0) return 0; if (n > len - pos) n = len - pos; pos += n; return n; }
        @Override public int available() { long a = len - pos; return a > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) a; }
        @Override public boolean markSupported() { return true; }
        @Override public void mark(int limit) { mark = pos; }
        @Override public void reset() { pos = mark; }
        @Override public void close() throws IOException { file.close(); }
    }
    public void close() {}
    // ---- generated by tools/compat/genstubs.py: signatures only
    public static abstract class AssetInputStream extends java.io.InputStream {
        protected AssetInputStream() { super(); }
        // ---- generated by tools/compat/fillmembers.py (AssetInputStream): the platform's members this class does not write (signatures only)
        public int getAssetInt() { return 0; }
        public long getNativeAsset() { return 0L; }
        // ---- end of generated members (AssetInputStream)
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private final java.util.HashMap<String, Object> megletFill = new java.util.HashMap<>();
    public static final int COOKIE_UNKNOWN = -1;
    public static java.lang.String FRAMEWORK_APK_PATH;
    public static void createSystemAssetsInZygoteLocked(boolean p0, java.lang.String p1) {}
    public static java.lang.String getAssetAllocations() { return null; }
    public static int getGlobalAssetCount() { return 0; }
    public static int getGlobalAssetManagerCount() { return 0; }
    public static android.content.res.AssetManager getSystem() { return null; }
    public void addApkKeys(java.util.List p0, boolean p1) {}
    public boolean containsAllocatedTable() { return false; }
    public android.content.res.ApkAssets[] getApkAssets() { return (android.content.res.ApkAssets[]) megletFill.get("ApkAssets"); }
    public java.lang.String getLastResourceResolution() { return null; }
    public java.util.List getLoaders() { return new java.util.ArrayList(); }
    public java.lang.String[] getNonSystemLocales() { return null; }
    public java.util.Map getOverlayableMap(java.lang.String p0) { return new java.util.HashMap(); }
    public java.lang.String getOverlayablesToString(java.lang.String p0) { return null; }
    public boolean isUpToDate() { return false; }
    public android.content.res.XmlResourceParser openXmlResourceParser(int p0, java.lang.String p1) { return null; }
    public android.content.res.XmlResourceParser openXmlResourceParser(int p0, java.lang.String p1, boolean p2) { return null; }
    public android.content.res.XmlResourceParser openXmlResourceParser(java.lang.String p0) { return null; }
    public void setApkAssets(android.content.res.ApkAssets[] p0, boolean p1) {}
    public void setConfiguration(int p0, int p1, java.lang.String p2, java.lang.String[] p3, int p4, int p5, int p6, int p7, int p8, int p9, int p10, int p11, int p12, int p13, int p14, int p15, int p16, int p17, int p18, int p19) {}
    public void setOverlayConstraints(int p0, int p1) {}
    public void setResourceResolutionLoggingEnabled(boolean p0) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static class Builder {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        public Builder() {}
        public android.content.res.AssetManager.Builder addApkAssets(android.content.res.ApkAssets p0) { return this; }
        public android.content.res.AssetManager.Builder addLoader(android.content.res.loader.ResourcesLoader p0) { return this; }
        public android.content.res.AssetManager build() { return null; }
        public android.content.res.AssetManager.Builder setNoInit() { return this; }
    }
    // ---- end of generated nested classes
}
