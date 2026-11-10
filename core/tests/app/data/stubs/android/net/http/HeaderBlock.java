package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class HeaderBlock {
    public abstract java.util.List<java.util.Map.Entry<String, String>> getAsList();
    public abstract java.util.Map<String, java.util.List<String>> getAsMap();
}
