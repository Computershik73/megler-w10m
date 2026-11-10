package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class HttpEngine {
    public static String getVersionString() { return null; }
    public abstract java.net.URLStreamHandlerFactory createUrlStreamHandlerFactory();
    public abstract BidirectionalStream.Builder newBidirectionalStreamBuilder(String url, java.util.concurrent.Executor executor, BidirectionalStream.Callback callback);
    public abstract UrlRequest.Builder newUrlRequestBuilder(String url, java.util.concurrent.Executor executor, UrlRequest.Callback callback);
    public abstract java.net.URLConnection openConnection(java.net.URL url) throws java.io.IOException;
    public abstract void shutdown();
    public static class Builder {
        public static final int HTTP_CACHE_DISABLED = 0, HTTP_CACHE_IN_MEMORY = 1, HTTP_CACHE_DISK_NO_HTTP = 2, HTTP_CACHE_DISK = 3;
        public Builder(android.content.Context context) {}
        public String getDefaultUserAgent() { return null; }
        public Builder setUserAgent(String userAgent) { return this; }
        public Builder setStoragePath(String value) { return this; }
        public Builder setEnableQuic(boolean value) { return this; }
        public Builder setEnableHttp2(boolean value) { return this; }
        public Builder setEnableBrotli(boolean value) { return this; }
        public Builder setEnableHttpCache(int cacheMode, long maxSize) { return this; }
        public Builder addQuicHint(String host, int port, int alternatePort) { return this; }
        public Builder addPublicKeyPins(String hostName, java.util.Set<byte[]> pinsSha256, boolean includeSubdomains, java.time.Instant expirationInstant) { return this; }
        public Builder setQuicOptions(QuicOptions quicOptions) { return this; }
        public Builder setDnsOptions(DnsOptions dnsOptions) { return this; }
        public Builder setConnectionMigrationOptions(ConnectionMigrationOptions connectionMigrationOptions) { return this; }
        public HttpEngine build() { return null; }
    }
}
