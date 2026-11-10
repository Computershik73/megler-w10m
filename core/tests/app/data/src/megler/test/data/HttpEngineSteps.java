package megler.test.data;

import android.content.Context;
import android.net.http.BidirectionalStream;
import android.net.http.CallbackException;
import android.net.http.ConnectionMigrationOptions;
import android.net.http.DnsOptions;
import android.net.http.HeaderBlock;
import android.net.http.HttpEngine;
import android.net.http.HttpException;
import android.net.http.NetworkException;
import android.net.http.QuicOptions;
import android.net.http.UploadDataProvider;
import android.net.http.UploadDataSink;
import android.net.http.UrlRequest;
import android.net.http.UrlResponseInfo;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.net.URLStreamHandler;
import java.net.URLStreamHandlerFactory;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLServerSocket;
import javax.net.ssl.SSLSocket;

/**
 * Android 14's HttpEngine (android.net.http) as an app uses it, the Google Play Store first among them: its builder and
 * options, UrlRequests (redirects, reads into the app's buffers, uploads of a fixed length and chunked, failures, cancel)
 * against an HTTP/1.1 server of the test's own on 127.0.0.1, the engine's URLConnections, and BidirectionalStreams -- what
 * gRPC's Cronet transport makes -- against an HTTP/2 server of the test's own over TLS: two at once on one connection, the
 * request's body and the response's flowing both ways, headers, trailers, flow control both ways past the windows, cancel
 * and the server's reset, and RFC 7541's examples of header compression (C.6, Huffman's code and the dynamic table, sent
 * padded and cut by a CONTINUATION). The HTTP/2 server writes its frames by hand and reads what the engine writes (literal
 * fields, plain strings): it shares no code with the engine (megler.http). Each check says what it saw in `detail`.
 */
final class HttpEngineSteps {
    String detail;
    private final Context context;
    private final SSLContext tls;
    private final ExecutorService callbacks = Executors.newSingleThreadExecutor(r -> new Thread(r, "engine-callbacks"));
    private final ExecutorService uploads = Executors.newSingleThreadExecutor(r -> new Thread(r, "engine-uploads"));
    private HttpEngine engine;
    private Http1Server http1;

    /** tls: a server's context with the test authority's certificate for localhost (the app's configuration trusts it). */
    HttpEngineSteps(Context context, SSLContext tls) {
        this.context = context;
        this.tls = tls;
    }

    private HttpEngine engine() {
        if (engine == null) engine = new HttpEngine.Builder(context).setUserAgent("megler-test/1").build();
        return engine;
    }

    private Http1Server http1() throws IOException {
        if (http1 == null) http1 = new Http1Server();
        return http1;
    }

    private interface Action {
        void run() throws Exception;
    }

    /** What an action threw, "Class: message", or "accepted". */
    private static String refused(Action a) {
        try {
            a.run();
            return "accepted";
        } catch (Exception e) {
            return e.getClass().getSimpleName() + ": " + e.getMessage();
        }
    }

    // ---- the engine

    /**
     * The builder as the Play Store drives it (QUIC hints, HTTP/2, the options of DNS, QUIC and connection migration) and
     * builds an engine, not null; its version four numbers and its user agent Android's; what Cronet refuses refused the
     * same way; shut down once, and not twice, and no request started after it.
     */
    boolean build() throws Exception {
        HttpEngine.Builder b = new HttpEngine.Builder(context);
        String agent = b.getDefaultUserAgent();
        QuicOptions quic = new QuicOptions.Builder().addAllowedQuicHost("www.example.com").build();
        b.setEnableHttp2(true).setEnableQuic(true).addQuicHint("www.example.com", 443, 443).setEnableBrotli(true)
                .setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_IN_MEMORY, 1 << 20)
                .setConnectionMigrationOptions(new ConnectionMigrationOptions.Builder().setDefaultNetworkMigration(ConnectionMigrationOptions.MIGRATION_OPTION_ENABLED).build())
                .setDnsOptions(new DnsOptions.Builder().setUseHttpStackDnsResolver(DnsOptions.DNS_OPTION_ENABLED).build())
                .setQuicOptions(quic);
        String storage = refused(() -> new HttpEngine.Builder(context).setStoragePath(new File(context.getCacheDir(), "no-such-folder").getPath()));
        String disk = refused(() -> new HttpEngine.Builder(context).setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_DISK, 1 << 20));
        String pin = refused(() -> new HttpEngine.Builder(context).addPublicKeyPins("www.example.com", Collections.singleton(new byte[3]), false, java.time.Instant.now()));
        String hint = refused(() -> new HttpEngine.Builder(context).addQuicHint("example.com/path", 443, 443));
        final HttpEngine e = b.build();
        String version = HttpEngine.getVersionString();
        e.shutdown();
        String again = refused(e::shutdown);
        String after = refused(() -> e.newUrlRequestBuilder("http://127.0.0.1:9/", callbacks, new Recorder()).build().start());
        detail = "engine=" + (e == null ? null : e.getClass().getName()) + " version=" + version + " agent=" + agent + " storage=" + storage
                + " disk=" + disk + " pin=" + pin + " hint=" + hint + " again=" + again + " after=" + after + " quic=" + quic.getAllowedQuicHosts();
        return e != null && version != null && version.matches("\\d+\\.\\d+\\.\\d+\\.\\d+")
                && Collections.singleton("www.example.com").equals(quic.getAllowedQuicHosts()) && !quic.hasInMemoryServerConfigsCacheSize()
                && agent.startsWith("AndroidHttpClient (Linux; U; Android ") && agent.endsWith("; Cronet/" + version + ")")
                && "IllegalArgumentException: Storage path must be set to existing directory".equals(storage)
                && "IllegalArgumentException: Storage path must be set".equals(disk)
                && "IllegalArgumentException: Public key pin is invalid".equals(pin)
                && hint.startsWith("IllegalArgumentException: Illegal QUIC Hint Host")
                && "IllegalStateException: Engine is shut down.".equals(again) && "IllegalStateException: Engine is shut down.".equals(after);
    }

    // ---- UrlRequest

    /** A UrlRequest's callbacks, said in order ("redirect 302 url;started 200;read 4;...;succeeded;"), its body kept. */
    static class Recorder implements UrlRequest.Callback {
        final StringBuilder events = new StringBuilder();
        final ByteArrayOutputStream body = new ByteArrayOutputStream();
        final CountDownLatch end = new CountDownLatch(1);
        final List<Integer> reads = new ArrayList<>();
        volatile UrlResponseInfo info;
        volatile HttpException error;
        volatile String thread;
        int readSize = 4;
        boolean follow = true, cancelAfterFirstRead;
        RuntimeException throwOnStart;

        @Override
        public void onRedirectReceived(UrlRequest request, UrlResponseInfo info, String url) {
            events.append("redirect ").append(info.getHttpStatusCode()).append(' ').append(url).append(';');
            if (follow) request.followRedirect();
            else request.cancel();
        }

        @Override
        public void onResponseStarted(UrlRequest request, UrlResponseInfo info) {
            this.info = info;
            thread = Thread.currentThread().getName();
            events.append("started ").append(info.getHttpStatusCode()).append(';');
            if (throwOnStart != null) throw throwOnStart;
            request.read(ByteBuffer.allocateDirect(readSize));
        }

        @Override
        public void onReadCompleted(UrlRequest request, UrlResponseInfo info, ByteBuffer buffer) {
            buffer.flip();
            byte[] b = new byte[buffer.remaining()];
            buffer.get(b);
            body.write(b, 0, b.length);
            reads.add(b.length);
            if (cancelAfterFirstRead) {
                events.append("read;");
                request.cancel();
                return;
            }
            buffer.clear();
            request.read(buffer);
        }

        @Override
        public void onSucceeded(UrlRequest request, UrlResponseInfo info) {
            this.info = info;
            events.append("succeeded;");
            end.countDown();
        }

        @Override
        public void onFailed(UrlRequest request, UrlResponseInfo info, HttpException error) {
            this.error = error;
            events.append("failed;");
            end.countDown();
        }

        @Override
        public void onCanceled(UrlRequest request, UrlResponseInfo info) {
            events.append("canceled;");
            end.countDown();
        }

        int total() {
            int n = 0;
            for (int r : reads) n += r;
            return n;
        }
    }

    /**
     * GET of a URL the server redirects (302, a relative Location) to its page, followed: the page read four bytes at a time
     * on the app's executor; the response as Android tells it (the URLs it went through, the status, the protocol, a header
     * given twice as two values found by any case, none of libcore's own X-Android-* headers); the engine's user agent and
     * the app's header sent; started twice refused; the status of a finished request INVALID. And a 404 and its body.
     */
    boolean request() throws Exception {
        Http1Server s = http1();
        String base = "http://127.0.0.1:" + s.port();
        Recorder r = new Recorder();
        final UrlRequest q = engine().newUrlRequestBuilder(base + "/redirect", callbacks, r).addHeader("X-Test", "one").build();
        q.start();
        String twice = refused(q::start);
        boolean ended = r.end.await(10, TimeUnit.SECONDS);
        final int[] status = { 99 };
        final CountDownLatch said = new CountDownLatch(1);
        q.getStatus(v -> {
            status[0] = v;
            said.countDown();
        });
        said.await(5, TimeUnit.SECONDS);
        UrlResponseInfo i = r.info;
        Map<String, List<String>> map = i == null ? Collections.<String, List<String>>emptyMap() : i.getHeaders().getAsMap();
        boolean android = false;
        if (i != null) for (Map.Entry<String, String> h : i.getHeaders().getAsList()) android |= h.getKey().startsWith("X-Android");
        Recorder m = new Recorder();
        m.readSize = 64;
        engine().newUrlRequestBuilder(base + "/missing", callbacks, m).build().start();
        boolean missing = m.end.await(10, TimeUnit.SECONDS);
        String sent = s.saw("GET /hello");
        boolean reads = r.reads.size() >= 2;
        for (int n : r.reads) reads &= n > 0 && n <= 4;
        detail = "ended=" + ended + " events=" + r.events + " reads=" + r.reads + " body=" + r.body + " chain=" + (i == null ? null : i.getUrlChain())
                + " status=" + (i == null ? 0 : i.getHttpStatusCode()) + " " + (i == null ? null : i.getHttpStatusText()) + " protocol=" + (i == null ? null : i.getNegotiatedProtocol())
                + " x-megler=" + map.get("x-megler") + " received=" + (i == null ? 0 : i.getReceivedByteCount()) + " thread=" + r.thread + " twice=" + twice
                + " done=" + q.isDone() + "/" + status[0] + " sent=" + sent + " missing=" + m.events + m.body;
        return ended && (r.events.toString().equals("redirect 302 " + base + "/hello;started 200;succeeded;")) && reads && r.total() == 7
                && "megler!".equals(r.body.toString("UTF-8")) && i.getUrlChain().equals(Arrays.asList(base + "/redirect", base + "/hello"))
                && (base + "/hello").equals(i.getUrl()) && i.getHttpStatusCode() == 200 && "OK".equals(i.getHttpStatusText())
                && "http/1.1".equals(i.getNegotiatedProtocol()) && Arrays.asList("one", "two").equals(map.get("X-MEGLER")) && !android
                && i.getReceivedByteCount() >= 7 && !i.wasCached() && "engine-callbacks".equals(r.thread)
                && "IllegalStateException: Request is already started.".equals(twice) && q.isDone() && status[0] == UrlRequest.Status.INVALID
                && sent != null && sent.contains(" agent=megler-test/1 ") && sent.contains(" x-test=one ")
                && missing && "started 404;succeeded;".equals(m.events.toString()) && "missing".equals(m.body.toString("UTF-8"));
    }

    /** An upload of the app's own, `step` bytes a read, of its length or chunked (the last chunk said by the provider). */
    static final class Provider extends UploadDataProvider {
        private final byte[] data;
        private final boolean chunked;
        private final int step;
        private int at;
        volatile boolean closed;
        IOException fail;

        Provider(String data, boolean chunked, int step) {
            this.data = data.getBytes(StandardCharsets.UTF_8);
            this.chunked = chunked;
            this.step = step;
        }

        @Override
        public long getLength() {
            return chunked ? -1 : data.length;
        }

        @Override
        public void read(UploadDataSink sink, ByteBuffer buffer) throws IOException {
            if (fail != null) throw fail;
            int n = Math.min(step, Math.min(buffer.remaining(), data.length - at));
            buffer.put(data, at, n);
            at += n;
            sink.onReadSucceeded(chunked && at == data.length);
        }

        @Override
        public void rewind(UploadDataSink sink) {
            at = 0;
            sink.onRewindSucceeded();
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    private static boolean closedSoon(Provider p) throws InterruptedException {
        for (int i = 0; i < 40 && !p.closed; i++) Thread.sleep(50);
        return p.closed;
    }

    /**
     * POSTs with an upload: thirteen bytes of a known length given five at a time, then a chunked one; each provider closed
     * when its request ends; an upload without a Content-Type refused at start, as Cronet refuses it; a 303 after a POST
     * followed as a GET without the body.
     */
    boolean upload() throws Exception {
        final Http1Server s = http1();
        final String base = "http://127.0.0.1:" + s.port();
        Provider fixed = new Provider("hello, upload", false, 5), chunks = new Provider("chunks of it", true, 4), other = new Provider("gone", false, 64);
        Recorder a = new Recorder(), b = new Recorder(), c = new Recorder();
        a.readSize = b.readSize = c.readSize = 256;
        engine().newUrlRequestBuilder(base + "/echo", callbacks, a).addHeader("Content-Type", "text/plain").setUploadDataProvider(fixed, uploads).build().start();
        boolean aEnded = a.end.await(10, TimeUnit.SECONDS);
        engine().newUrlRequestBuilder(base + "/echo", callbacks, b).addHeader("Content-Type", "text/plain").setUploadDataProvider(chunks, uploads).build().start();
        boolean bEnded = b.end.await(10, TimeUnit.SECONDS);
        String untyped = refused(() -> engine().newUrlRequestBuilder(base + "/echo", callbacks, new Recorder()).setUploadDataProvider(new Provider("x", false, 1), uploads).build().start());
        engine().newUrlRequestBuilder(base + "/see-other", callbacks, c).addHeader("Content-Type", "text/plain").setUploadDataProvider(other, uploads).build().start();
        boolean cEnded = c.end.await(10, TimeUnit.SECONDS);
        boolean closed = closedSoon(fixed) && closedSoon(chunks) && closedSoon(other);
        String fixedSaw = s.saw("POST /echo length=13"), chunkSaw = s.saw("POST /echo chunked"), getSaw = s.saw("GET /method");
        detail = "fixed=" + a.events + a.body + " | " + fixedSaw + " chunked=" + b.events + b.body + " | " + chunkSaw + " untyped=" + untyped
                + " see-other=" + c.events + c.body + " | " + getSaw + " closed=" + closed;
        return aEnded && bEnded && cEnded && "started 200;succeeded;".equals(a.events.toString()) && "13 hello, upload".equals(a.body.toString("UTF-8"))
                && fixedSaw != null && fixedSaw.contains(" type=text/plain ") && fixedSaw.endsWith(" body=hello, upload")
                && "started 200;succeeded;".equals(b.events.toString()) && "12 chunks of it".equals(b.body.toString("UTF-8"))
                && chunkSaw != null && chunkSaw.endsWith(" body=chunks of it")
                && "IllegalArgumentException: Requests with upload data must have a Content-Type.".equals(untyped)
                && ("redirect 303 " + base + "/method;started 200;succeeded;").equals(c.events.toString()) && "GET 0".equals(c.body.toString("UTF-8"))
                && getSaw != null && getSaw.contains(" length=null ") && !getSaw.contains("type=text/plain") && closed;
    }

    /**
     * What fails, fails as Android's engine says: a refused connection (NetworkException ERROR_CONNECTION_REFUSED, Chromium's
     * name in the message, not to be tried again at once); an exception the app's callback throws (CallbackException, that
     * exception its cause); one its upload throws (the same); a request canceled in the middle of its body (onCanceled, done).
     */
    boolean failures() throws Exception {
        Http1Server s = http1();
        String base = "http://127.0.0.1:" + s.port();
        ServerSocket closed = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
        int port = closed.getLocalPort();
        closed.close();
        Recorder refused = new Recorder(), thrown = new Recorder(), upload = new Recorder(), canceled = new Recorder();
        engine().newUrlRequestBuilder("http://127.0.0.1:" + port + "/", callbacks, refused).build().start();
        thrown.throwOnStart = new IllegalStateException("boom");
        engine().newUrlRequestBuilder(base + "/hello", callbacks, thrown).build().start();
        Provider bad = new Provider("never", false, 2);
        bad.fail = new IOException("no data");
        engine().newUrlRequestBuilder(base + "/echo", callbacks, upload).addHeader("Content-Type", "text/plain").setUploadDataProvider(bad, uploads).build().start();
        canceled.cancelAfterFirstRead = true;
        canceled.readSize = 8;
        UrlRequest q = engine().newUrlRequestBuilder(base + "/slow", callbacks, canceled).build();
        q.start();
        boolean ended = refused.end.await(10, TimeUnit.SECONDS) & thrown.end.await(10, TimeUnit.SECONDS) & upload.end.await(10, TimeUnit.SECONDS)
                & canceled.end.await(10, TimeUnit.SECONDS);
        HttpException e = refused.error, t = thrown.error, u = upload.error;
        NetworkException n = e instanceof NetworkException ? (NetworkException) e : null;
        detail = "ended=" + ended + " refused=" + refused.events + e + (n == null ? "" : " code=" + n.getErrorCode() + " retry=" + n.isImmediatelyRetryable())
                + " thrown=" + thrown.events + t + " cause=" + (t == null ? null : t.getCause()) + " upload=" + upload.events + u + " cause=" + (u == null ? null : u.getCause())
                + " canceled=" + canceled.events + " done=" + q.isDone();
        return ended && "failed;".equals(refused.events.toString()) && n != null && n.getErrorCode() == NetworkException.ERROR_CONNECTION_REFUSED
                && !n.isImmediatelyRetryable() && n.getMessage().contains("net::ERR_CONNECTION_REFUSED") && n.getCause() != null
                && "started 200;failed;".equals(thrown.events.toString()) && t instanceof CallbackException && t.getCause() instanceof IllegalStateException
                && "boom".equals(t.getCause().getMessage()) && "failed;".equals(upload.events.toString()) && u instanceof CallbackException
                && u.getCause() instanceof IOException && "no data".equals(u.getCause().getMessage())
                && "started 200;read;canceled;".equals(canceled.events.toString()) && q.isDone();
    }

    /**
     * The engine's URLConnection (Android's HTTP client) gets the page; so does a URL made with the handler of its
     * URLStreamHandlerFactory, which hands nothing but http and https; a protocol the engine has not is refused.
     */
    boolean connection() throws Exception {
        Http1Server s = http1();
        String page = "http://127.0.0.1:" + s.port() + "/hello";
        HttpURLConnection c = (HttpURLConnection) engine().openConnection(new URL(page));
        c.setConnectTimeout(5000);
        c.setReadTimeout(5000);
        int code = c.getResponseCode();
        String body = text(c.getInputStream());
        c.disconnect();
        URLStreamHandlerFactory f = engine().createUrlStreamHandlerFactory();
        URLStreamHandler h = f.createURLStreamHandler("http");
        boolean ftp = f.createURLStreamHandler("ftp") == null;
        HttpURLConnection d = (HttpURLConnection) new URL(null, page, h).openConnection();
        d.setConnectTimeout(5000);
        d.setReadTimeout(5000);
        int code2 = d.getResponseCode();
        String body2 = text(d.getInputStream());
        d.disconnect();
        String other = refused(() -> engine().openConnection(new URL("ftp://127.0.0.1/file")));
        detail = "code=" + code + " body=" + body + " handler=" + (h != null) + " ftp=" + ftp + " code2=" + code2 + " body2=" + body2 + " other=" + other;
        return code == 200 && "megler!".equals(body) && h != null && ftp && code2 == 200 && "megler!".equals(body2)
                && other.startsWith("UnsupportedOperationException");
    }

    /** An HTTP/1.1 server of the steps' own on 127.0.0.1: a request a connection, answered by its path, the connection
     *  closed. What it was sent is kept ("GET /hello length=null type=null agent=... x-test=... body=..."). */
    static final class Http1Server implements Runnable {
        final ServerSocket socket;
        final List<String> seen = Collections.synchronizedList(new ArrayList<String>());

        Http1Server() throws IOException {
            socket = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
            Thread t = new Thread(this, "http1-server");
            t.setDaemon(true);
            t.start();
        }

        int port() {
            return socket.getLocalPort();
        }

        String saw(String start) {
            synchronized (seen) {
                for (String s : seen) if (s.startsWith(start)) return s;
            }
            return null;
        }

        @Override
        public void run() {
            while (!socket.isClosed()) {
                try {
                    final Socket c = socket.accept();
                    Thread t = new Thread(() -> serve(c), "http1-connection");
                    t.setDaemon(true);
                    t.start();
                } catch (IOException e) {
                    return;
                }
            }
        }

        private static String line(InputStream in) throws IOException {
            StringBuilder b = new StringBuilder();
            for (int c; (c = in.read()) >= 0 && c != '\n'; ) if (c != '\r') b.append((char) c);
            return b.toString();
        }

        private void serve(Socket c) {
            try (Socket s = c) {
                s.setSoTimeout(15000);
                InputStream in = new BufferedInputStream(s.getInputStream());
                String[] request = line(in).split(" ");
                HashMap<String, String> h = new HashMap<>();
                for (String l; !(l = line(in)).isEmpty(); ) {
                    int k = l.indexOf(':');
                    if (k > 0) h.put(l.substring(0, k).trim().toLowerCase(Locale.ROOT), l.substring(k + 1).trim());
                }
                ByteArrayOutputStream body = new ByteArrayOutputStream();
                boolean chunked = "chunked".equalsIgnoreCase(h.get("transfer-encoding"));
                if (chunked) {
                    for (int n; (n = Integer.parseInt(line(in).trim(), 16)) > 0; ) {
                        for (int i = 0; i < n; i++) body.write(in.read());
                        line(in);
                    }
                    line(in);
                } else if (h.containsKey("content-length")) {
                    for (int i = Integer.parseInt(h.get("content-length")); i > 0; i--) body.write(in.read());
                }
                String method = request[0], path = request[1], text = body.toString("UTF-8");
                seen.add(method + " " + path + " " + (chunked ? "chunked" : "length=" + h.get("content-length")) + " type=" + h.get("content-type")
                        + " agent=" + h.get("user-agent") + " x-test=" + h.get("x-test") + " body=" + text);
                OutputStream o = s.getOutputStream();
                switch (path) {
                    case "/redirect":
                        o.write(head("302 Found", "Location: /hello\r\nContent-Length: 0"));
                        break;
                    case "/hello":
                        o.write(head("200 OK", "Content-Type: text/plain\r\nX-Megler: one\r\nX-Megler: two\r\nContent-Length: 7"));
                        o.write("megler!".getBytes(StandardCharsets.UTF_8));
                        break;
                    case "/echo": {
                        byte[] answer = (body.size() + " " + text).getBytes(StandardCharsets.UTF_8);
                        o.write(head("200 OK", "Content-Type: text/plain\r\nContent-Length: " + answer.length));
                        o.write(answer);
                        break;
                    }
                    case "/see-other":
                        o.write(head("303 See Other", "Location: /method\r\nContent-Length: 0"));
                        break;
                    case "/method": {
                        byte[] answer = (method + " " + body.size()).getBytes(StandardCharsets.UTF_8);
                        o.write(head("200 OK", "Content-Type: text/plain\r\nContent-Length: " + answer.length));
                        o.write(answer);
                        break;
                    }
                    case "/slow":
                        /* Ten bytes of a hundred thousand, then nothing until the client gives up. */
                        o.write(head("200 OK", "Content-Type: application/octet-stream\r\nContent-Length: 100000"));
                        o.write(new byte[10]);
                        o.flush();
                        while (in.read() >= 0) { }
                        break;
                    default:
                        o.write(head("404 Not Found", "Content-Type: text/plain\r\nContent-Length: 7"));
                        o.write("missing".getBytes(StandardCharsets.UTF_8));
                }
                o.flush();
            } catch (IOException | RuntimeException e) {
                // the client went
            }
        }

        private static byte[] head(String status, String fields) {
            return ("HTTP/1.1 " + status + "\r\n" + fields + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.ISO_8859_1);
        }
    }

    // ---- BidirectionalStream

    /** A stream's callbacks, in order ("ready", "headers 200", "written", "read", "trailers", "read end", "succeeded"):
     *  what it writes once ready (the last with endOfStream, then one flush), what it read, kept or summed (CRC32). */
    static final class StreamRecorder implements BidirectionalStream.Callback {
        final List<String> events = Collections.synchronizedList(new ArrayList<String>());
        final ByteArrayOutputStream body = new ByteArrayOutputStream();
        final CountDownLatch end = new CountDownLatch(1);
        final byte[][] writes;
        final CRC32 crc = new CRC32();
        volatile long total;
        volatile UrlResponseInfo info;
        volatile HeaderBlock trailers;
        volatile HttpException error;
        boolean cancelOnHeaders, keep = true;
        RuntimeException throwOnReady;
        int readSize = 16 * 1024;

        StreamRecorder(byte[]... writes) {
            this.writes = writes;
        }

        @Override
        public void onStreamReady(BidirectionalStream stream) {
            events.add("ready");
            if (throwOnReady != null) throw throwOnReady;
            for (int i = 0; i < writes.length; i++) {
                ByteBuffer b = ByteBuffer.allocateDirect(writes[i].length);
                b.put(writes[i]);
                b.flip();
                stream.write(b, i == writes.length - 1);
            }
            if (writes.length > 0) stream.flush();
        }

        @Override
        public void onResponseHeadersReceived(BidirectionalStream stream, UrlResponseInfo info) {
            this.info = info;
            events.add("headers " + info.getHttpStatusCode());
            if (cancelOnHeaders) {
                stream.cancel();
                return;
            }
            stream.read(ByteBuffer.allocateDirect(readSize));
        }

        @Override
        public void onReadCompleted(BidirectionalStream stream, UrlResponseInfo info, ByteBuffer buffer, boolean endOfStream) {
            buffer.flip();
            byte[] b = new byte[buffer.remaining()];
            buffer.get(b);
            if (keep) body.write(b, 0, b.length);
            crc.update(b);
            total += b.length;
            if (endOfStream) {
                events.add("read end");
                return;
            }
            if (events.isEmpty() || !"read".equals(events.get(events.size() - 1))) events.add("read");
            buffer.clear();
            stream.read(buffer);
        }

        @Override
        public void onWriteCompleted(BidirectionalStream stream, UrlResponseInfo info, ByteBuffer buffer, boolean endOfStream) {
            events.add(endOfStream ? "written end" : "written");
        }

        @Override
        public void onResponseTrailersReceived(BidirectionalStream stream, UrlResponseInfo info, HeaderBlock trailers) {
            this.trailers = trailers;
            events.add("trailers");
        }

        @Override
        public void onSucceeded(BidirectionalStream stream, UrlResponseInfo info) {
            this.info = info;
            events.add("succeeded");
            end.countDown();
        }

        @Override
        public void onFailed(BidirectionalStream stream, UrlResponseInfo info, HttpException error) {
            this.error = error;
            events.add("failed");
            end.countDown();
        }

        @Override
        public void onCanceled(BidirectionalStream stream, UrlResponseInfo info) {
            events.add("canceled");
            end.countDown();
        }

        String text() {
            return new String(body.toByteArray(), StandardCharsets.UTF_8);
        }

        /** Whether `a` came before `b` among the events (both there). */
        boolean before(String a, String b) {
            int i = events.indexOf(a), j = events.indexOf(b);
            return i >= 0 && j >= 0 && i < j;
        }
    }

    private static String fields(HeaderBlock block) {
        if (block == null) return null;
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : block.getAsList()) b.append(b.length() == 0 ? "" : "; ").append(e.getKey()).append(": ").append(e.getValue());
        return b.toString();
    }

    private static String text(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[256];
        for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    /**
     * Two streams at once to the HTTP/2 server's echo, each writing twice and ending, as gRPC does: both on one connection
     * (the server is connected to once), the response's headers before its body (the server sends them first), the echo
     * read whole, the trailers (grpc-status 0) before the end, both writes completed and the last said as the end; the
     * protocol h2. And a GET, which sends no body: its HEADERS end the request. The server saw the engine's settings (no
     * push, a window of a megabyte for each stream), the pseudo-headers, the app's headers in lower case and its user agent.
     */
    boolean stream() throws Exception {
        H2Server server = new H2Server(tls, 0);
        try {
            String echo = "https://localhost:" + server.port() + "/echo";
            StreamRecorder a = new StreamRecorder(utf8("hello "), utf8("world")), b = new StreamRecorder(utf8("second "), utf8("stream")), g = new StreamRecorder();
            BidirectionalStream sa = engine().newBidirectionalStreamBuilder(echo, callbacks, a).addHeader("Content-Type", "application/grpc")
                    .addHeader("te", "trailers").addHeader("X-Megler", "a").setPriority(3).build();
            BidirectionalStream sb = engine().newBidirectionalStreamBuilder(echo, callbacks, b).addHeader("content-type", "application/grpc")
                    .addHeader("X-Megler", "b").setDelayRequestHeadersUntilFirstFlushEnabled(true).build();
            BidirectionalStream sg = engine().newBidirectionalStreamBuilder(echo + "-get", callbacks, g).setHttpMethod("GET").build();
            sa.start();
            sb.start();
            sg.start();
            boolean ended = a.end.await(15, TimeUnit.SECONDS) & b.end.await(15, TimeUnit.SECONDS) & g.end.await(15, TimeUnit.SECONDS);
            UrlResponseInfo ia = a.info, ig = g.info;
            String headersA = server.saw("headers ", ":path=/echo", "x-megler=a"), headersG = server.saw("headers ", ":path=/echo-get");
            detail = "ended=" + ended + " a=" + a.events + " '" + a.text() + "' trailers=" + fields(a.trailers) + " b=" + b.events + " '" + b.text() + "' g=" + g.events
                    + " '" + g.text() + "' status=" + (ia == null ? 0 : ia.getHttpStatusCode()) + " protocol=" + (ia == null ? null : ia.getNegotiatedProtocol())
                    + " headers=" + (ia == null ? null : fields(ia.getHeaders())) + " connections=" + server.connections + " server=" + server.log;
            return ended && "hello world".equals(a.text()) && "second stream".equals(b.text()) && "got".equals(g.text())
                    && a.events.get(0).equals("ready") && a.events.get(a.events.size() - 1).equals("succeeded") && a.before("headers 200", "read end")
                    && a.before("trailers", "read end") && a.events.contains("written") && a.events.contains("written end") && !a.events.contains("failed")
                    && b.events.contains("written end") && b.events.get(b.events.size() - 1).equals("succeeded") && b.trailers != null
                    && "grpc-status: 0; grpc-message: fine".equals(fields(a.trailers)) && ia.getHttpStatusCode() == 200 && "h2".equals(ia.getNegotiatedProtocol())
                    && "content-type: application/grpc; x-echo-of: /echo; x-megler-was: a".equals(fields(ia.getHeaders()))
                    && g.events.equals(Arrays.asList("ready", "headers 200", "read", "trailers", "read end", "succeeded")) && ig != null && ig.getHttpStatusCode() == 200
                    && server.connections.get() == 1 && server.log.contains("alpn h2") && server.log.contains("client-settings push=0 window=1048576")
                    && headersA != null && headersA.contains(" :method=POST ") && headersA.contains(" :scheme=https ")
                    && headersA.contains(" :authority=localhost:" + server.port() + " ") && headersA.contains(" content-type=application/grpc ")
                    && headersA.contains(" te=trailers ") && headersA.contains(" user-agent=megler-test/1") && !headersA.endsWith(" end")
                    && headersG != null && headersG.contains(" :method=GET ") && headersG.endsWith(" end");
        } finally {
            server.close();
        }
    }

    /**
     * Flow control both ways. Up: 200 000 bytes in one write to a server whose streams take 16 KB at a time
     * (SETTINGS_INITIAL_WINDOW_SIZE) and which gives each DATA back as it reads it: all of it arrives, CRC32 and all, and the
     * engine never had more out than the windows let it. Down: 1.25 MB, past the megabyte this side lets a stream receive
     * ahead of the app: the server waits for the engine's WINDOW_UPDATE, which it sends as the app reads.
     */
    boolean streamFlow() throws Exception {
        H2Server server = new H2Server(tls, 16384);
        try {
            byte[] big = new byte[200000];
            for (int i = 0; i < big.length; i++) big[i] = (byte) (i * 7 + 3);
            CRC32 sum = new CRC32();
            sum.update(big);
            StreamRecorder up = new StreamRecorder(big), down = new StreamRecorder(utf8("1310720"));
            down.keep = false;
            down.readSize = 64 * 1024;
            engine().newBidirectionalStreamBuilder("https://localhost:" + server.port() + "/upload", callbacks, up).build().start();
            boolean upEnded = up.end.await(15, TimeUnit.SECONDS);
            engine().newBidirectionalStreamBuilder("https://localhost:" + server.port() + "/download", callbacks, down).build().start();
            boolean downEnded = down.end.await(15, TimeUnit.SECONDS);
            CRC32 want = new CRC32();
            for (int i = 0; i < 1310720; i++) want.update((i * 31 + 7) & 0xff);
            String windows = server.saw("window ");
            detail = "up=" + upEnded + " " + up.events + " '" + up.text() + "' want '" + big.length + " " + sum.getValue() + "' down=" + downEnded + " "
                    + down.events.size() + " events, last " + (down.events.isEmpty() ? null : down.events.get(down.events.size() - 1)) + " total=" + down.total
                    + " crc=" + down.crc.getValue() + "/" + want.getValue() + " server=" + server.log;
            return upEnded && downEnded && (big.length + " " + sum.getValue()).equals(up.text()) && "succeeded".equals(up.events.get(up.events.size() - 1))
                    && down.total == 1310720 && down.crc.getValue() == want.getValue() && "succeeded".equals(down.events.get(down.events.size() - 1))
                    && windows != null && !server.log.toString().contains("overflow");
        } finally {
            server.close();
        }
    }

    /**
     * A stream the app cancels once the response's headers are there: onCanceled, done, and the server sees RST_STREAM
     * CANCEL (8). One the server resets (INTERNAL_ERROR): onFailed with a NetworkException that says so. One to an http URL:
     * refused, as Cronet's bidirectional streams are HTTPS. An exception from the app's onStreamReady: onFailed with a
     * CallbackException, and the stream reset at the server.
     */
    boolean streamCancel() throws Exception {
        H2Server server = new H2Server(tls, 0);
        try {
            String base = "https://localhost:" + server.port();
            StreamRecorder held = new StreamRecorder(), reset = new StreamRecorder(utf8("x")), plain = new StreamRecorder(), thrown = new StreamRecorder();
            held.cancelOnHeaders = true;
            thrown.throwOnReady = new IllegalStateException("not ready");
            BidirectionalStream h = engine().newBidirectionalStreamBuilder(base + "/hold", callbacks, held).build();
            h.start();
            boolean heldEnded = held.end.await(15, TimeUnit.SECONDS);
            engine().newBidirectionalStreamBuilder(base + "/reset", callbacks, reset).build().start();
            engine().newBidirectionalStreamBuilder("http://localhost:" + server.port() + "/echo", callbacks, plain).build().start();
            engine().newBidirectionalStreamBuilder(base + "/hold", callbacks, thrown).build().start();
            boolean ended = reset.end.await(15, TimeUnit.SECONDS) & plain.end.await(15, TimeUnit.SECONDS) & thrown.end.await(15, TimeUnit.SECONDS);
            for (int i = 0; i < 40 && server.count("rst ") < 2; i++) Thread.sleep(50);
            HttpException r = reset.error, p = plain.error, t = thrown.error;
            detail = "held=" + heldEnded + " " + held.events + " done=" + h.isDone() + " reset=" + reset.events + " " + r + " plain=" + plain.events + " " + p
                    + " thrown=" + thrown.events + " " + t + " server=" + server.log;
            return heldEnded && ended && held.events.equals(Arrays.asList("ready", "headers 200", "canceled")) && h.isDone()
                    && server.log.contains("rst 1 8") && r instanceof NetworkException && ((NetworkException) r).getErrorCode() == NetworkException.ERROR_OTHER
                    && r.getMessage().contains("INTERNAL_ERROR") && reset.events.get(reset.events.size() - 1).equals("failed")
                    && p instanceof NetworkException && p.getMessage().contains("net::ERR_DISALLOWED_URL_SCHEME") && plain.events.equals(Collections.singletonList("failed"))
                    && t instanceof CallbackException && t.getCause() instanceof IllegalStateException && thrown.events.equals(Arrays.asList("ready", "failed"))
                    && server.count("rst ") == 2;
        } finally {
            server.close();
        }
    }

    /**
     * Header compression as a server's encoder uses it: three responses on one connection, RFC 7541's C.6.1 to C.6.3 --
     * literals added to the dynamic table, references to it, strings in Huffman's code -- after a size update of the table
     * to 256 bytes, which makes C.6.2's and C.6.3's entries push older ones out; the second sent padded, the third cut in two
     * by a CONTINUATION. Each status and header as the RFC has them.
     */
    boolean hpack() throws Exception {
        H2Server server = new H2Server(tls, 0);
        HttpEngine own = new HttpEngine.Builder(context).build();
        try {
            String url = "https://localhost:" + server.port() + "/hpack";
            String[] want = {
                "302 cache-control: private; date: Mon, 21 Oct 2013 20:13:21 GMT; location: https://www.example.com",
                "307 cache-control: private; date: Mon, 21 Oct 2013 20:13:21 GMT; location: https://www.example.com",
                "200 cache-control: private; date: Mon, 21 Oct 2013 20:13:22 GMT; location: https://www.example.com; content-encoding: gzip; "
                        + "set-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" };
            StringBuilder got = new StringBuilder();
            boolean held = true;
            for (int i = 0; i < 3; i++) {
                StreamRecorder r = new StreamRecorder();
                own.newBidirectionalStreamBuilder(url, callbacks, r).setHttpMethod("GET").build().start();
                boolean ended = r.end.await(15, TimeUnit.SECONDS);
                String seen = r.info == null ? "failed " + r.error : r.info.getHttpStatusCode() + " " + fields(r.info.getHeaders());
                got.append(i == 0 ? "" : " | ").append(seen);
                held &= ended && want[i].equals(seen) && "succeeded".equals(r.events.get(r.events.size() - 1));
            }
            detail = "got=" + got + " connections=" + server.connections + " server=" + server.log;
            return held && server.connections.get() == 1;
        } finally {
            server.close();
            try {
                own.shutdown();
            } catch (IllegalStateException e) {
                detail += " shutdown=" + e.getMessage();
            }
        }
    }

    // ---- an HTTP/2 server of the steps' own

    /** RFC 7541's C.6.1 to C.6.3 (responses, Huffman's code, a 256-byte dynamic table). */
    private static final String C61 = "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b97c8e9ae82ae43d3";
    private static final String C62 = "4883640effc1c0bf";
    private static final String C63 = "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7821dd7f2e6c7b335dfdfcd5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007";

    private static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    /**
     * RFC 7540 by hand, on 127.0.0.1 over TLS with ALPN h2: SETTINGS (and SETTINGS_INITIAL_WINDOW_SIZE where `window`), a
     * thread for each connection reading its frames. Paths: /echo answers its headers at once, echoes each DATA (the
     * first padded) and ends with trailers; /echo-get, "got" and trailers; /upload counts what comes, giving each DATA
     * back to the windows, and answers its length and CRC32; /download sends the bytes its request asks for, as the
     * engine's windows allow; /hold answers headers and nothing else; /reset resets the stream; /hpack the RFC's blocks in
     * turn. `log` keeps what it was sent: "client-settings ...", "headers <id> name=value ... [end]", "rst <id> <code>",
     * "window <id>", and "overflow" if the engine ever sent more than a window allowed.
     */
    static final class H2Server implements Runnable {
        final SSLServerSocket socket;
        final int window;
        final List<String> log = Collections.synchronizedList(new ArrayList<String>());
        final AtomicInteger connections = new AtomicInteger();

        H2Server(SSLContext tls, int window) throws IOException {
            socket = (SSLServerSocket) tls.getServerSocketFactory().createServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
            this.window = window;
            Thread t = new Thread(this, "h2-server");
            t.setDaemon(true);
            t.start();
        }

        int port() {
            return socket.getLocalPort();
        }

        void close() {
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }

        /** The first line of the log that starts with `start` and has every one of `parts` (as " part " or at its end). */
        String saw(String start, String... parts) {
            synchronized (log) {
                next:
                for (String l : log) {
                    if (!l.startsWith(start)) continue;
                    for (String p : parts) if (!(l + " ").contains(" " + p + " ")) continue next;
                    return l;
                }
            }
            return null;
        }

        int count(String start) {
            int n = 0;
            synchronized (log) {
                for (String l : log) if (l.startsWith(start)) n++;
            }
            return n;
        }

        @Override
        public void run() {
            while (!socket.isClosed()) {
                try {
                    final SSLSocket c = (SSLSocket) socket.accept();
                    connections.incrementAndGet();
                    Thread t = new Thread(() -> new H2Connection(this, c).serve(), "h2-connection");
                    t.setDaemon(true);
                    t.start();
                } catch (IOException e) {
                    return;
                }
            }
        }
    }

    /** One stream at the server: its request's headers, its body so far, the windows. */
    private static final class ServerStream {
        final int id;
        final String path;
        final Map<String, String> fields;
        long sendWindow, received, granted;
        final CRC32 crc = new CRC32();

        ServerStream(int id, Map<String, String> fields, long sendWindow) {
            this.id = id;
            this.fields = fields;
            this.path = fields.get(":path");
            this.sendWindow = sendWindow;
        }
    }

    private static final class H2Connection {
        private static final int DATA = 0, HEADERS = 1, RST_STREAM = 3, SETTINGS = 4, PING = 6, GOAWAY = 7, WINDOW_UPDATE = 8, CONTINUATION = 9;
        private final H2Server server;
        private final SSLSocket socket;
        private DataInputStream in;
        private OutputStream out;
        private final HashMap<Integer, ServerStream> streams = new HashMap<>();
        private long sendWindow = 65535, received, granted;
        private int clientWindow = 65535, hpacks;
        private boolean firstEchoData = true;

        H2Connection(H2Server server, SSLSocket socket) {
            this.server = server;
            this.socket = socket;
        }

        void serve() {
            try {
                socket.setSoTimeout(30000);
                SSLParameters p = socket.getSSLParameters();
                p.setApplicationProtocols(new String[] { "h2" });
                socket.setSSLParameters(p);
                socket.startHandshake();
                server.log.add("alpn " + socket.getApplicationProtocol());
                in = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
                out = new BufferedOutputStream(socket.getOutputStream());
                byte[] preface = new byte[24];
                in.readFully(preface);
                if (!"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n".equals(new String(preface, StandardCharsets.ISO_8859_1))) {
                    server.log.add("no preface");
                    return;
                }
                ByteArrayOutputStream settings = new ByteArrayOutputStream();
                setting(settings, 3, 100);
                if (server.window > 0) setting(settings, 4, server.window);
                frame(SETTINGS, 0, 0, settings.toByteArray());
                while (true) {
                    int length = in.readUnsignedByte() << 16 | in.readUnsignedByte() << 8 | in.readUnsignedByte();
                    int type = in.readUnsignedByte(), flags = in.readUnsignedByte(), id = in.readInt() & 0x7fffffff;
                    byte[] payload = new byte[length];
                    in.readFully(payload);
                    switch (type) {
                        case SETTINGS: settings(flags, payload); break;
                        case WINDOW_UPDATE: windowUpdate(id, payload); break;
                        case HEADERS: headers(flags, id, payload); break;
                        case DATA: data(flags, id, payload); break;
                        case RST_STREAM: server.log.add("rst " + id + " " + int32(payload, 0)); break;
                        case PING:
                            if ((flags & 1) == 0) frame(PING, 1, 0, payload);
                            break;
                        case GOAWAY:
                            server.log.add("goaway " + int32(payload, 4));
                            return;
                        default:
                            break;
                    }
                }
            } catch (IOException | RuntimeException e) {
                // the engine closed the connection
            } finally {
                try {
                    socket.close();
                } catch (IOException ignored) {
                }
            }
        }

        private static int int32(byte[] b, int at) {
            return (b[at] & 0xff) << 24 | (b[at + 1] & 0xff) << 16 | (b[at + 2] & 0xff) << 8 | (b[at + 3] & 0xff);
        }

        private static void setting(ByteArrayOutputStream o, int id, int value) {
            o.write(id >> 8);
            o.write(id);
            o.write(value >>> 24);
            o.write(value >>> 16);
            o.write(value >>> 8);
            o.write(value);
        }

        private void frame(int type, int flags, int id, byte[] payload) throws IOException {
            synchronized (this) {
                out.write(payload.length >>> 16);
                out.write(payload.length >>> 8);
                out.write(payload.length);
                out.write(type);
                out.write(flags);
                out.write(id >>> 24);
                out.write(id >>> 16);
                out.write(id >>> 8);
                out.write(id);
                out.write(payload);
                out.flush();
            }
        }

        private void windowUpdate(int id, int increment) throws IOException {
            frame(WINDOW_UPDATE, 0, id, new byte[] { (byte) (increment >>> 24), (byte) (increment >>> 16), (byte) (increment >>> 8), (byte) increment });
        }

        private void settings(int flags, byte[] payload) throws IOException {
            if ((flags & 1) != 0) return;
            int push = -1, window = -1;
            for (int i = 0; i + 6 <= payload.length; i += 6) {
                int id = (payload[i] & 0xff) << 8 | (payload[i + 1] & 0xff), value = int32(payload, i + 2);
                if (id == 2) push = value;
                if (id == 4) window = value;
            }
            if (window >= 0) {
                synchronized (this) {
                    for (ServerStream s : streams.values()) s.sendWindow += window - clientWindow;
                    clientWindow = window;
                    notifyAll();
                }
            }
            server.log.add("client-settings push=" + push + " window=" + window);
            frame(SETTINGS, 1, 0, new byte[0]);
        }

        private void windowUpdate(int id, byte[] payload) {
            int increment = int32(payload, 0) & 0x7fffffff;
            synchronized (this) {
                if (id == 0) {
                    sendWindow += increment;
                } else {
                    ServerStream s = streams.get(id);
                    if (s != null) s.sendWindow += increment;
                    if (server.saw("window " + id) == null) server.log.add("window " + id);
                }
                notifyAll();
            }
        }

        /** The engine's fields: literals, not indexed or never indexed, with names of their own and plain strings. */
        private static Map<String, String> decode(byte[] b) throws IOException {
            Map<String, String> out = new java.util.LinkedHashMap<>();
            int[] at = { 0 };
            while (at[0] < b.length) {
                int x = b[at[0]] & 0xff;
                if ((x & 0xe0) != 0 || (x & 0x0f) != 0) throw new IOException("a field this server does not read: 0x" + Integer.toHexString(x));
                at[0]++;
                String name = string(b, at), value = string(b, at);
                out.put(name, value);
            }
            return out;
        }

        private static String string(byte[] b, int[] at) throws IOException {
            if ((b[at[0]] & 0x80) != 0) throw new IOException("a string in Huffman's code, which the engine does not write");
            int n = b[at[0]++] & 0x7f;
            if (n == 0x7f) {
                int shift = 0, x;
                do {
                    x = b[at[0]++] & 0xff;
                    n += (x & 0x7f) << shift;
                    shift += 7;
                } while ((x & 0x80) != 0);
            }
            String s = new String(b, at[0], n, StandardCharsets.UTF_8);
            at[0] += n;
            return s;
        }

        /** Response fields: ":status 200" indexed (0x88), the rest literals not indexed, with plain strings. */
        private static byte[] block(String... namesAndValues) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            for (int i = 0; i < namesAndValues.length; i += 2) {
                if (":status".equals(namesAndValues[i]) && "200".equals(namesAndValues[i + 1])) {
                    o.write(0x88);
                    continue;
                }
                o.write(0);
                for (String s : new String[] { namesAndValues[i], namesAndValues[i + 1] }) {
                    byte[] b = s.getBytes(StandardCharsets.UTF_8);
                    o.write(b.length);
                    o.write(b, 0, b.length);
                }
            }
            return o.toByteArray();
        }

        private void headers(int flags, int id, byte[] payload) throws IOException {
            ByteArrayOutputStream fragment = new ByteArrayOutputStream();
            fragment.write(payload, 0, payload.length);
            while ((flags & 4) == 0) {
                int length = in.readUnsignedByte() << 16 | in.readUnsignedByte() << 8 | in.readUnsignedByte();
                int type = in.readUnsignedByte();
                flags = (flags & 1) | in.readUnsignedByte();
                in.readInt();
                byte[] more = new byte[length];
                in.readFully(more);
                if (type != CONTINUATION) throw new IOException("a header block cut by another frame");
                fragment.write(more, 0, length);
            }
            boolean end = (flags & 1) != 0;
            Map<String, String> fields = decode(fragment.toByteArray());
            StringBuilder line = new StringBuilder("headers ").append(id);
            for (Map.Entry<String, String> f : fields.entrySet()) line.append(' ').append(f.getKey()).append('=').append(f.getValue());
            server.log.add(line.append(end ? " end" : "").toString());
            ServerStream s;
            synchronized (this) {
                s = new ServerStream(id, fields, clientWindow);
                streams.put(id, s);
            }
            switch (String.valueOf(s.path)) {
                case "/echo":
                    frame(HEADERS, 4, id, block(":status", "200", "content-type", "application/grpc", "x-echo-of", "/echo", "x-megler-was", fields.get("x-megler")));
                    if (end) frame(HEADERS, 5, id, block("grpc-status", "0", "grpc-message", "fine"));
                    break;
                case "/echo-get":
                    frame(HEADERS, 4, id, block(":status", "200"));
                    frame(DATA, 0, id, "got".getBytes(StandardCharsets.UTF_8));
                    frame(HEADERS, 5, id, block("grpc-status", "0"));
                    break;
                case "/hold":
                    frame(HEADERS, 4, id, block(":status", "200"));
                    break;
                case "/reset":
                    frame(RST_STREAM, 0, id, new byte[] { 0, 0, 0, 2 });
                    break;
                case "/hpack":
                    hpack(id);
                    break;
                default:
                    break;
            }
        }

        /** C.6.1 after a size update of the table to 256 bytes; C.6.2 padded; C.6.3 in a HEADERS and a CONTINUATION. */
        private void hpack(int id) throws IOException {
            int n = hpacks++;
            if (n == 0) {
                byte[] block = hex("3fe101" + C61);
                frame(HEADERS, 5, id, block);
            } else if (n == 1) {
                byte[] block = hex(C62), padded = new byte[1 + block.length + 5];
                padded[0] = 5;
                System.arraycopy(block, 0, padded, 1, block.length);
                frame(HEADERS, 5 | 8, id, padded);
            } else {
                byte[] block = hex(C63);
                frame(HEADERS, 1, id, Arrays.copyOfRange(block, 0, 20));
                frame(CONTINUATION, 4, id, Arrays.copyOfRange(block, 20, block.length));
            }
        }

        private void data(int flags, int id, byte[] payload) throws IOException {
            int start = 0, length = payload.length;
            if ((flags & 8) != 0) {
                start = 1;
                length -= 1 + (payload[0] & 0xff);
            }
            ServerStream s;
            boolean overflow;
            synchronized (this) {
                s = streams.get(id);
                received += payload.length;
                overflow = received - granted > 65535;
                if (s != null) {
                    s.received += payload.length;
                    overflow |= s.received - s.granted > Math.max(65535, server.window);
                }
            }
            if (overflow) server.log.add("overflow " + id);
            if (s == null) return;
            /* Every DATA given back to the windows as it is read. */
            if (payload.length > 0) {
                synchronized (this) {
                    granted += payload.length;
                    s.granted += payload.length;
                }
                windowUpdate(0, payload.length);
                windowUpdate(id, payload.length);
            }
            byte[] bytes = Arrays.copyOfRange(payload, start, start + length);
            boolean end = (flags & 1) != 0;
            switch (String.valueOf(s.path)) {
                case "/echo":
                    if (bytes.length > 0) {
                        if (firstEchoData) {
                            firstEchoData = false;
                            byte[] padded = new byte[1 + bytes.length + 3];
                            padded[0] = 3;
                            System.arraycopy(bytes, 0, padded, 1, bytes.length);
                            frame(DATA, 8, id, padded);
                        } else {
                            frame(DATA, 0, id, bytes);
                        }
                    }
                    if (end) frame(HEADERS, 5, id, block("grpc-status", "0", "grpc-message", "fine"));
                    break;
                case "/upload":
                    s.crc.update(bytes);
                    s.fields.put("count", String.valueOf(Long.parseLong(s.fields.containsKey("count") ? s.fields.get("count") : "0") + bytes.length));
                    if (end) {
                        frame(HEADERS, 4, id, block(":status", "200"));
                        frame(DATA, 1, id, (s.fields.get("count") + " " + s.crc.getValue()).getBytes(StandardCharsets.UTF_8));
                    }
                    break;
                case "/download":
                    if (end) download(s, Integer.parseInt(new String(bytes, StandardCharsets.UTF_8).trim()));
                    break;
                default:
                    break;
            }
        }

        /** `total` bytes ((i * 31 + 7) & 0xff), in frames of 16 KB at most, each waiting for the engine's windows. */
        private void download(final ServerStream s, final int total) throws IOException {
            frame(HEADERS, 4, s.id, block(":status", "200", "content-type", "application/octet-stream"));
            Thread t = new Thread(() -> {
                try {
                    int at = 0;
                    while (at < total) {
                        int n;
                        synchronized (H2Connection.this) {
                            long deadline = System.currentTimeMillis() + 10000;
                            while ((sendWindow <= 0 || s.sendWindow <= 0) && System.currentTimeMillis() < deadline) H2Connection.this.wait(100);
                            n = (int) Math.min(Math.min(16384, total - at), Math.min(sendWindow, s.sendWindow));
                            if (n <= 0) {
                                server.log.add("download stalled at " + at);
                                return;
                            }
                            sendWindow -= n;
                            s.sendWindow -= n;
                        }
                        byte[] b = new byte[n];
                        for (int i = 0; i < n; i++) b[i] = (byte) ((at + i) * 31 + 7);
                        at += n;
                        frame(DATA, at == total ? 1 : 0, s.id, b);
                    }
                    server.log.add("download sent " + total);
                } catch (IOException | InterruptedException e) {
                    server.log.add("download failed " + e);
                }
            }, "h2-download");
            t.setDaemon(true);
            t.start();
        }
    }
}
