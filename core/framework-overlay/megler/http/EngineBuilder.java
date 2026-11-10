package megler.http;

import android.content.Context;
import android.net.http.ConnectionMigrationOptions;
import android.net.http.DnsOptions;
import android.net.http.HttpEngine;
import android.net.http.IHttpEngineBuilder;
import android.net.http.QuicOptions;
import android.os.Build;
import java.io.File;
import java.time.Instant;
import java.util.Locale;
import java.util.Set;

/**
 * What HttpEngine.Builder hands its calls to (Android 14 has Cronet's builder there): the options, checked as Cronet checks
 * them, and the engine they build (megler.http.Engine). Those of QUIC, the cache, Brotli, DNS and connection migration are
 * kept and change nothing; public key pins are kept and said in the log, not enforced.
 */
public final class EngineBuilder extends IHttpEngineBuilder {
    private static boolean pinsSaid;

    private final Context context;
    private String userAgent, storagePath;
    private boolean quic, http2 = true, brotli, pinBypass = true;
    private int cacheMode = HttpEngine.Builder.HTTP_CACHE_DISABLED;
    private long cacheMax;
    private QuicOptions quicOptions;
    private DnsOptions dnsOptions;
    private ConnectionMigrationOptions migrationOptions;

    public EngineBuilder(Context context) {
        this.context = context;
    }

    String userAgent() {
        return userAgent != null ? userAgent : getDefaultUserAgent();
    }

    /** Android 14's own (Cronet's for the platform): "AndroidHttpClient (Linux; U; Android 14; en_US; model; Build/id; Cronet/v)". */
    @Override
    public String getDefaultUserAgent() {
        StringBuilder b = new StringBuilder("AndroidHttpClient (Linux; U; Android ").append(Build.VERSION.RELEASE).append("; ").append(Locale.getDefault());
        if (Build.MODEL != null && !Build.MODEL.isEmpty()) b.append("; ").append(Build.MODEL);
        if (Build.ID != null && !Build.ID.isEmpty()) b.append("; Build/").append(Build.ID);
        return b.append("; Cronet/").append(Engine.VERSION).append(')').toString();
    }

    @Override
    public IHttpEngineBuilder setUserAgent(String userAgent) {
        this.userAgent = userAgent;
        return this;
    }

    @Override
    public IHttpEngineBuilder setStoragePath(String value) {
        if (value == null || !new File(value).isDirectory()) throw new IllegalArgumentException("Storage path must be set to existing directory");
        storagePath = value;
        return this;
    }

    @Override
    public IHttpEngineBuilder setEnableQuic(boolean value) {
        quic = value;
        return this;
    }

    @Override
    public IHttpEngineBuilder setEnableHttp2(boolean value) {
        http2 = value;
        return this;
    }

    @Override
    public IHttpEngineBuilder setEnableBrotli(boolean value) {
        brotli = value;
        return this;
    }

    @Override
    public IHttpEngineBuilder setEnableHttpCache(int cacheMode, long maxSize) {
        if (cacheMode < HttpEngine.Builder.HTTP_CACHE_DISABLED || cacheMode > HttpEngine.Builder.HTTP_CACHE_DISK) throw new IllegalArgumentException("Unknown cache mode " + cacheMode);
        if ((cacheMode == HttpEngine.Builder.HTTP_CACHE_DISK || cacheMode == HttpEngine.Builder.HTTP_CACHE_DISK_NO_HTTP) && storagePath == null) {
            throw new IllegalArgumentException("Storage path must be set");
        }
        this.cacheMode = cacheMode;
        this.cacheMax = maxSize;
        return this;
    }

    @Override
    public IHttpEngineBuilder addQuicHint(String host, int port, int alternatePort) {
        if (host == null || host.contains("/")) throw new IllegalArgumentException("Illegal QUIC Hint Host: " + host);
        return this;
    }

    @Override
    public IHttpEngineBuilder addPublicKeyPins(String hostName, Set<byte[]> pinsSha256, boolean includeSubdomains, Instant expirationInstant) {
        if (hostName == null) throw new NullPointerException("The hostname cannot be null");
        if (pinsSha256 == null) throw new NullPointerException("The set of SHA256 pins cannot be null");
        if (expirationInstant == null) throw new NullPointerException("The pin expiration date cannot be null");
        if (hostName.matches("^[0-9\\.]*$")) {
            throw new IllegalArgumentException("Hostname " + hostName + " is illegal. A hostname should not consist of digits and/or dots only.");
        }
        for (byte[] pin : pinsSha256) {
            if (pin == null || pin.length != 32) throw new IllegalArgumentException("Public key pin is invalid");
        }
        synchronized (EngineBuilder.class) {
            if (!pinsSaid) {
                pinsSaid = true;
                android.util.Log.w("Megler", "HttpEngine: the app pins public keys (" + hostName + " and maybe more): kept, not enforced here");
            }
        }
        return this;
    }

    @Override
    public IHttpEngineBuilder setEnablePublicKeyPinningBypassForLocalTrustAnchors(boolean value) {
        pinBypass = value;
        return this;
    }

    @Override
    public IHttpEngineBuilder setQuicOptions(QuicOptions options) {
        quicOptions = options;
        return this;
    }

    @Override
    public IHttpEngineBuilder setDnsOptions(DnsOptions options) {
        dnsOptions = options;
        return this;
    }

    @Override
    public IHttpEngineBuilder setConnectionMigrationOptions(ConnectionMigrationOptions options) {
        migrationOptions = options;
        return this;
    }

    @Override
    public HttpEngine build() {
        return new Engine(this);
    }
}
