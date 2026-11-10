package megler.http;

import android.net.Network;
import android.net.http.BidirectionalStream;
import android.net.http.HttpEngine;
import android.net.http.UrlRequest;
import java.io.IOException;
import java.net.Proxy;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandler;
import java.net.URLStreamHandlerFactory;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicInteger;
import javax.net.ssl.HttpsURLConnection;

/**
 * Android 14's HttpEngine (android.net.http), as Megler has it: Android's is Cronet in the Tethering module, Chromium's
 * network stack over its own native library, which Megler does not carry. This one is Java over what Android 9's libcore
 * already has:
 *
 *   - UrlRequest (megler.http.Request): Android's own HTTP client, libcore's HttpURLConnection (okhttp 2.7, HTTP/1.1), its
 *     redirects, upload and reads driven as Cronet's callbacks say;
 *   - BidirectionalStream (megler.http.Stream): an HTTP/2 client of Megler's own (megler.http.H2, Hpack, Connections) over
 *     conscrypt's TLS with ALPN, as gRPC's Cronet transport needs: a request's body and its response's flow both ways at
 *     once, and the response's trailers apart from its headers;
 *   - openConnection and createUrlStreamHandlerFactory: libcore's HttpURLConnection, through libcore's own handlers.
 *
 * There is no QUIC, no HTTP cache, no Brotli and no public key pinning here: the options for them are kept and do nothing
 * (QUIC hints and an HTTP cache are what an engine may do without, and an app's pins are said in the log). The version
 * (getVersionString) is "1.0.0.0", lower than any Cronet's: Cronet's own provider list puts an HttpEngine of a lower version
 * than the one it prefers below the Cronet an app carries (HttpEngineNativeProvider), so where an app has both, Chromium's
 * stack is taken. Google Play Store 53.7 asks for HttpEngine itself on Android 14 when a server flag of its says so
 * (NetworkOptimizationsAutogen's enable_bypass_sdk_extensions_check_for_platform_cronet_on_u) and uses it for all its
 * requests and its gRPC: before this engine, HttpEngine.Builder.build() was a stub's null, and every one failed.
 */
public final class Engine extends HttpEngine {
    public static final String VERSION = "1.0.0.0";
    private static final AtomicInteger built = new AtomicInteger(), failures = new AtomicInteger();

    final String userAgent;
    final Connections connections = new Connections(this);
    private final Object lock = new Object();
    private int running;
    private boolean shutDown;
    private ExecutorService io;
    private ScheduledExecutorService timer;

    Engine(EngineBuilder b) {
        this.userAgent = b.userAgent();
        if (built.getAndIncrement() == 0) {
            android.util.Log.i("Megler", "HttpEngine: the app made one: Megler's own (HTTP/2 of its own for bidirectional streams, "
                    + "Android's HttpURLConnection for requests; no QUIC, no cache)");
        }
    }

    /** The threads requests and streams wait for the network on, as many as they need, kept a minute when idle. */
    ExecutorService io() {
        synchronized (lock) {
            if (io == null) io = Executors.newCachedThreadPool(threads("MeglerHttp"));
            return io;
        }
    }

    ScheduledExecutorService timer() {
        synchronized (lock) {
            if (timer == null) timer = Executors.newSingleThreadScheduledExecutor(threads("MeglerHttp timer"));
            return timer;
        }
    }

    private static ThreadFactory threads(final String name) {
        final AtomicInteger n = new AtomicInteger();
        return r -> {
            Thread t = new Thread(r, name + " #" + n.incrementAndGet());
            t.setDaemon(true);
            return t;
        };
    }

    /** A request or stream starts: refused once the engine is shut down, as Cronet refuses it. */
    void started() {
        synchronized (lock) {
            if (shutDown) throw new IllegalStateException("Engine is shut down.");
            running++;
        }
    }

    void finished() {
        synchronized (lock) {
            running--;
        }
    }

    /** A network failure of a request or a stream, said in the log twenty times a process at most (the host alone: a URL
     *  can carry what is not the log's). */
    static void sayFailure(String what, String host, Throwable e) {
        if (failures.getAndIncrement() < 20) android.util.Log.w("Megler", "HttpEngine: " + what + " to " + host + " failed: " + e.getMessage());
    }

    @Override
    public UrlRequest.Builder newUrlRequestBuilder(String url, Executor executor, UrlRequest.Callback callback) {
        return new Request.Builder(this, url, executor, callback);
    }

    @Override
    public BidirectionalStream.Builder newBidirectionalStreamBuilder(String url, Executor executor, BidirectionalStream.Callback callback) {
        return new Stream.Builder(this, url, executor, callback);
    }

    /** Android's HTTP client's connection, as Cronet gives its own: for http and https alone. */
    @Override
    public URLConnection openConnection(URL url) throws IOException {
        String protocol = url.getProtocol();
        if (!"http".equals(protocol) && !"https".equals(protocol)) throw new UnsupportedOperationException("Unexpected protocol:" + protocol);
        URLConnection c = libcore(url);
        if (c instanceof HttpsURLConnection) Connections.Tls.platform((HttpsURLConnection) c);
        return c;
    }

    @Override
    public URLStreamHandlerFactory createUrlStreamHandlerFactory() {
        return protocol -> {
            if (!"http".equals(protocol) && !"https".equals(protocol)) return null;
            final int port = "http".equals(protocol) ? 80 : 443;
            return new URLStreamHandler() {
                @Override
                protected URLConnection openConnection(URL u) throws IOException {
                    return Engine.this.openConnection(u);
                }

                @Override
                protected URLConnection openConnection(URL u, Proxy proxy) {
                    throw new UnsupportedOperationException();
                }

                @Override
                protected int getDefaultPort() {
                    return port;
                }
            };
        };
    }

    @Override
    public void bindToNetwork(Network network) {
        /* One network here, the phone's: there is nothing to bind to. */
    }

    @Override
    public void shutdown() {
        ExecutorService pool;
        ScheduledExecutorService t;
        synchronized (lock) {
            if (shutDown) throw new IllegalStateException("Engine is shut down.");
            if (running != 0) throw new IllegalStateException("Cannot shutdown with running requests.");
            shutDown = true;
            pool = io;
            t = timer;
        }
        connections.shutdown();
        if (pool != null) pool.shutdown();
        if (t != null) t.shutdownNow();
    }

    // ---- libcore's own handlers: a URL made with one opens Android's HTTP client, whatever factory the app has installed
    //      (this engine's own, from createUrlStreamHandlerFactory, would otherwise come back here for ever).

    private static URLStreamHandler http, https;

    static URLConnection libcore(URL url) throws IOException {
        URLStreamHandler h = handler(url.getProtocol());
        return h != null ? new URL(null, url.toExternalForm(), h).openConnection() : new URL(url.toExternalForm()).openConnection();
    }

    private static synchronized URLStreamHandler handler(String protocol) {
        boolean secure = "https".equals(protocol);
        if (secure ? https == null : http == null) {
            try {
                URLStreamHandler h = (URLStreamHandler) Class.forName(secure ? "com.android.okhttp.HttpsHandler" : "com.android.okhttp.HttpHandler").newInstance();
                if (secure) https = h;
                else http = h;
            } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
                return null;
            }
        }
        return secure ? https : http;
    }
}
