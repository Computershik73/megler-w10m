package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step http-engine calls of Android 14's class. */
public class QuicOptions {
    public java.util.Set<String> getAllowedQuicHosts() { return null; }
    public boolean hasInMemoryServerConfigsCacheSize() { return false; }
    public static final class Builder {
        public Builder() {}
        public Builder addAllowedQuicHost(String host) { return this; }
        public QuicOptions build() { return null; }
    }
}
