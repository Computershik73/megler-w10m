package android.content.res.loader;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step resources-loader calls of Android 11's class. */
public class ResourcesProvider implements AutoCloseable {
    public static ResourcesProvider loadFromApk(android.os.ParcelFileDescriptor fd) throws java.io.IOException { return null; }
    public void close() {}
}
