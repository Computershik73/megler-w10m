package android.net.http;

import android.content.Context;
import android.net.Network;
import java.io.IOException;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandlerFactory;
import java.time.Instant;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * Megler: Android 14's HttpEngine, the platform's HTTP stack for apps (Cronet, in Android's Tethering module), with Megler's
 * own engine behind its Builder (megler.http.Engine says what it is). The generated stub this replaces built null, and the
 * Google Play Store, which takes HttpEngine itself on Android 14 when a server flag of its says so, failed every request and
 * every gRPC call on that null ("Attempt to invoke virtual method ... newBidirectionalStreamBuilder ... on a null object").
 * The API is Android 14's, the Builder handing each call to the hidden IHttpEngineBuilder as Android's does.
 */
public abstract class HttpEngine {
    /** @hide */
    protected HttpEngine() {}

    /** @hide Android's hidden form of new Builder(context). */
    public static Builder builder(Context context) {
        return new Builder(context);
    }

    /** The engine's version: Megler's own, below every Cronet's (megler.http.Engine.VERSION says why). */
    public static String getVersionString() {
        return megler.http.Engine.VERSION;
    }

    /** @hide Android loads Cronet's library ahead with it; there is nothing to load here. */
    public static void preload() {}

    public void bindToNetwork(Network network) {
        throw new UnsupportedOperationException("Not implemented.");
    }

    public abstract URLStreamHandlerFactory createUrlStreamHandlerFactory();

    public abstract BidirectionalStream.Builder newBidirectionalStreamBuilder(String url, Executor executor, BidirectionalStream.Callback callback);

    public abstract UrlRequest.Builder newUrlRequestBuilder(String url, Executor executor, UrlRequest.Callback callback);

    public abstract URLConnection openConnection(URL url) throws IOException;

    public abstract void shutdown();

    public static class Builder {
        public static final int HTTP_CACHE_DISABLED = 0;
        public static final int HTTP_CACHE_IN_MEMORY = 1;
        public static final int HTTP_CACHE_DISK_NO_HTTP = 2;
        public static final int HTTP_CACHE_DISK = 3;

        /** @hide */
        protected IHttpEngineBuilder mBuilderDelegate;

        public Builder(Context context) {
            this(new megler.http.EngineBuilder(context));
        }

        /** @hide */
        public Builder(IHttpEngineBuilder builderDelegate) {
            mBuilderDelegate = builderDelegate;
        }

        public String getDefaultUserAgent() {
            return mBuilderDelegate.getDefaultUserAgent();
        }

        public Builder setUserAgent(String userAgent) {
            mBuilderDelegate.setUserAgent(userAgent);
            return this;
        }

        public Builder setStoragePath(String value) {
            mBuilderDelegate.setStoragePath(value);
            return this;
        }

        public Builder setEnableQuic(boolean value) {
            mBuilderDelegate.setEnableQuic(value);
            return this;
        }

        public Builder setEnableHttp2(boolean value) {
            mBuilderDelegate.setEnableHttp2(value);
            return this;
        }

        public Builder setEnableBrotli(boolean value) {
            mBuilderDelegate.setEnableBrotli(value);
            return this;
        }

        public Builder setEnableHttpCache(int cacheMode, long maxSize) {
            mBuilderDelegate.setEnableHttpCache(cacheMode, maxSize);
            return this;
        }

        public Builder addQuicHint(String host, int port, int alternatePort) {
            mBuilderDelegate.addQuicHint(host, port, alternatePort);
            return this;
        }

        public Builder addPublicKeyPins(String hostName, Set<byte[]> pinsSha256, boolean includeSubdomains, Instant expirationInstant) {
            mBuilderDelegate.addPublicKeyPins(hostName, pinsSha256, includeSubdomains, expirationInstant);
            return this;
        }

        public Builder setEnablePublicKeyPinningBypassForLocalTrustAnchors(boolean value) {
            mBuilderDelegate.setEnablePublicKeyPinningBypassForLocalTrustAnchors(value);
            return this;
        }

        public Builder setQuicOptions(QuicOptions quicOptions) {
            mBuilderDelegate.setQuicOptions(quicOptions);
            return this;
        }

        public Builder setDnsOptions(DnsOptions dnsOptions) {
            mBuilderDelegate.setDnsOptions(dnsOptions);
            return this;
        }

        public Builder setConnectionMigrationOptions(ConnectionMigrationOptions connectionMigrationOptions) {
            mBuilderDelegate.setConnectionMigrationOptions(connectionMigrationOptions);
            return this;
        }

        public HttpEngine build() {
            return mBuilderDelegate.build();
        }
    }
}
