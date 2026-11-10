package android.net.http;

import java.time.Instant;
import java.util.Set;

/**
 * Megler: @hide, as on Android: the engine's builder that HttpEngine.Builder hands every call to (Android 14 has Cronet's
 * there). Megler's is megler.http.EngineBuilder. The generated stub this replaces had no methods.
 */
public abstract class IHttpEngineBuilder {
    protected IHttpEngineBuilder() {}

    public abstract String getDefaultUserAgent();

    public abstract IHttpEngineBuilder setUserAgent(String userAgent);

    public abstract IHttpEngineBuilder setStoragePath(String value);

    public abstract IHttpEngineBuilder setEnableQuic(boolean value);

    public abstract IHttpEngineBuilder setEnableHttp2(boolean value);

    public abstract IHttpEngineBuilder setEnableBrotli(boolean value);

    public abstract IHttpEngineBuilder setEnableHttpCache(int cacheMode, long maxSize);

    public abstract IHttpEngineBuilder addQuicHint(String host, int port, int alternatePort);

    public abstract IHttpEngineBuilder addPublicKeyPins(String hostName, Set<byte[]> pinsSha256, boolean includeSubdomains, Instant expirationInstant);

    public abstract IHttpEngineBuilder setEnablePublicKeyPinningBypassForLocalTrustAnchors(boolean value);

    public abstract IHttpEngineBuilder setQuicOptions(QuicOptions options);

    public abstract IHttpEngineBuilder setDnsOptions(DnsOptions options);

    public abstract IHttpEngineBuilder setConnectionMigrationOptions(ConnectionMigrationOptions options);

    public abstract HttpEngine build();
}
