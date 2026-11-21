package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine and sdk-extensions call of Android 14's class. */
public final class DnsOptions {
    public static final int DNS_OPTION_ENABLED = 1;
    public static final int DNS_OPTION_DISABLED = 2;
    public static final class Builder {
        public Builder() {}
        public Builder setUseHttpStackDnsResolver(int option) { return this; }
        public Builder setStaleDns(int option) { return this; }
        public Builder setPersistHostCache(int option) { return this; }
        public Builder setPersistHostCachePeriod(java.time.Duration period) { return this; }
        public Builder setPreestablishConnectionsToStaleDnsResults(int option) { return this; }
        public Builder setStaleDnsOptions(StaleDnsOptions options) { return this; }
        public DnsOptions build() { return null; }
    }
    public static class StaleDnsOptions {
        public static final class Builder {
            public Builder() {}
            public Builder setFreshLookupTimeout(java.time.Duration timeout) { return this; }
            public Builder setMaxExpiredDelay(java.time.Duration delay) { return this; }
            public Builder setAllowCrossNetworkUsage(int option) { return this; }
            public Builder setUseStaleOnNameNotResolved(int option) { return this; }
            public StaleDnsOptions build() { return null; }
        }
    }
}
