package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step http-engine calls of Android 14's class. */
public final class DnsOptions {
    public static final int DNS_OPTION_ENABLED = 1;
    public static final class Builder {
        public Builder() {}
        public Builder setUseHttpStackDnsResolver(int option) { return this; }
        public DnsOptions build() { return null; }
    }
}
