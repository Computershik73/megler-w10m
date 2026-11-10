package megler.http;

import android.net.Network;
import android.net.http.HeaderBlock;
import android.net.http.HttpException;
import android.net.http.NetworkException;
import android.net.http.UploadDataProvider;
import android.net.http.UploadDataSink;
import android.net.http.UrlRequest;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import javax.net.ssl.HttpsURLConnection;

/**
 * A UrlRequest of Megler's HttpEngine: Android's own HTTP client (libcore's HttpURLConnection) on the engine's threads,
 * the app's callbacks on its executor in Cronet's order and states -- onRedirectReceived and followRedirect (the client's
 * own redirects are off; 303, and 301 and 302 after a POST, go on as GET without the body, 307 and 308 rewind the upload),
 * onResponseStarted, a read into the app's buffer at a time and onReadCompleted, onSucceeded at the body's end, onFailed
 * with a NetworkException for the network's failures and a CallbackException for the app's, onCanceled after cancel().
 * The upload is the app's UploadDataProvider read on its executor into the request's body, of a fixed length or chunked.
 * Accept-Encoding is the client's (gzip, decoded for the app), as Cronet ignores the app's; libcore's own X-Android-*
 * headers are not the response's.
 */
final class Request extends UrlRequest {
    static final class Builder extends UrlRequest.Builder {
        private final Engine engine;
        private final String url;
        private final Executor executor;
        private final Callback callback;
        private String method;
        private final ArrayList<Map.Entry<String, String>> headers = new ArrayList<>();
        private boolean cacheDisabled, directAllowed, hasTag, hasUid;
        private int priority = REQUEST_PRIORITY_MEDIUM, tag, uid;
        private UploadDataProvider upload;
        private Executor uploadExecutor;

        Builder(Engine engine, String url, Executor executor, Callback callback) {
            if (url == null) throw new NullPointerException("URL is required.");
            if (callback == null) throw new NullPointerException("Callback is required.");
            if (executor == null) throw new NullPointerException("Executor is required.");
            this.engine = engine;
            this.url = url;
            this.executor = executor;
            this.callback = callback;
        }

        @Override
        public UrlRequest.Builder setHttpMethod(String method) {
            if (method == null) throw new NullPointerException("Method is required.");
            this.method = method;
            return this;
        }

        @Override
        public UrlRequest.Builder addHeader(String name, String value) {
            if (name == null) throw new NullPointerException("Invalid header name.");
            if (value == null) throw new NullPointerException("Invalid header value.");
            if ("Accept-Encoding".equalsIgnoreCase(name)) {
                synchronized (Request.class) {
                    if (!encodingSaid) {
                        encodingSaid = true;
                        android.util.Log.w("Megler", "HttpEngine: It's not necessary to set Accept-Encoding on requests - cronet will do this automatically for you, and setting it yourself has no effect.");
                    }
                }
                return this;
            }
            headers.add(Info.entry(name, value));
            return this;
        }

        @Override public UrlRequest.Builder setCacheDisabled(boolean disable) { cacheDisabled = disable; return this; }
        @Override public UrlRequest.Builder setPriority(int priority) { this.priority = priority; return this; }
        @Override public UrlRequest.Builder setDirectExecutorAllowed(boolean allow) { directAllowed = allow; return this; }
        @Override public UrlRequest.Builder setTrafficStatsTag(int tag) { hasTag = true; this.tag = tag; return this; }
        @Override public UrlRequest.Builder setTrafficStatsUid(int uid) { hasUid = true; this.uid = uid; return this; }
        @Override public UrlRequest.Builder bindToNetwork(Network network) { return this; }

        @Override
        public UrlRequest.Builder setUploadDataProvider(UploadDataProvider provider, Executor executor) {
            if (provider == null) throw new NullPointerException("Invalid UploadDataProvider.");
            if (executor == null) throw new NullPointerException("Invalid UploadDataProvider Executor.");
            if (method == null) method = "POST";
            upload = provider;
            uploadExecutor = executor;
            return this;
        }

        @Override
        public UrlRequest build() {
            return new Request(this);
        }
    }

    private static final int NOT_STARTED = 0, STARTED = 1, REDIRECT_RECEIVED = 2, AWAITING_FOLLOW_REDIRECT = 3, AWAITING_READ = 4,
            READING = 5, COMPLETE = 6, ERROR = 7, CANCELLED = 8;
    private static final int MAX_REDIRECTS = 20;
    private static boolean encodingSaid;

    private final Engine engine;
    private final Executor executor;
    private final Callback callback;
    private final String initialMethod;
    private final List<Map.Entry<String, String>> headers;
    private final boolean cacheDisabled, directAllowed, hasTag, hasUid;
    private final int priority, tag, uid;
    private final UploadDataProvider upload;
    private final Executor uploadExecutor;

    private final Object lock = new Object();
    private int state = NOT_STARTED, status = Status.INVALID;
    private String method, url, pendingRedirect;
    private boolean sendBody, bodySent, uploadClosed;
    private int redirectCode, redirects;
    private final ArrayList<String> chain = new ArrayList<>();
    private HttpURLConnection conn;
    private InputStream body;
    private Info info;
    private byte[] scratch;
    private final UploadSink sink = new UploadSink();

    private Request(Builder b) {
        engine = b.engine;
        executor = b.executor;
        callback = b.callback;
        initialMethod = b.method == null ? "GET" : b.method;
        headers = new ArrayList<>(b.headers);
        cacheDisabled = b.cacheDisabled;
        directAllowed = b.directAllowed;
        hasTag = b.hasTag;
        hasUid = b.hasUid;
        priority = b.priority;
        tag = b.tag;
        uid = b.uid;
        upload = b.upload;
        uploadExecutor = b.uploadExecutor;
        method = initialMethod;
        url = b.url;
        sendBody = upload != null;
        chain.add(url);
    }

    // ---- the request's life

    @Override
    public void start() {
        synchronized (lock) {
            if (state != NOT_STARTED) throw new IllegalStateException("Request is already started.");
            if (upload != null && !hasHeader("Content-Type")) throw new IllegalArgumentException("Requests with upload data must have a Content-Type.");
            engine.started();
            state = STARTED;
            status = Status.CONNECTING;
        }
        io(this::connect);
    }

    private boolean hasHeader(String name) {
        for (Map.Entry<String, String> h : headers) if (name.equalsIgnoreCase(h.getKey())) return true;
        return false;
    }

    private void io(Runnable r) {
        try {
            engine.io().execute(r);
        } catch (RejectedExecutionException e) {
            fail(Failures.network("UrlRequest", new IOException("the engine is shut down", e), false));
        }
    }

    /** Connects (again, after a redirect), sends the request and its body, and reads the response's status and headers. */
    private void connect() {
        String target, verb;
        boolean withBody, again;
        synchronized (lock) {
            if (state != STARTED) return;
            target = url;
            verb = method;
            withBody = sendBody;
            again = bodySent;
        }
        HttpURLConnection c = null;
        boolean connecting = true;
        try {
            URL u = new URL(target);
            if (!"http".equals(u.getProtocol()) && !"https".equals(u.getProtocol())) {
                fail(new Failures.Network("Exception in UrlRequest: net::ERR_DISALLOWED_URL_SCHEME (" + u.getProtocol() + ")", null, NetworkException.ERROR_OTHER));
                return;
            }
            c = (HttpURLConnection) Engine.libcore(u);
            synchronized (lock) {
                if (state != STARTED) return;
                conn = c;
            }
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setDoInput(true);
            if (c instanceof HttpsURLConnection) Connections.Tls.platform((HttpsURLConnection) c);
            c.setRequestMethod(verb);
            boolean agent = false;
            for (Map.Entry<String, String> h : headers) {
                if (!withBody && upload != null && "Content-Type".equalsIgnoreCase(h.getKey())) continue;
                if ("User-Agent".equalsIgnoreCase(h.getKey())) agent = true;
                c.addRequestProperty(h.getKey(), h.getValue());
            }
            if (!agent) c.setRequestProperty("User-Agent", engine.userAgent);
            if (withBody) {
                synchronized (lock) {
                    status = Status.SENDING_REQUEST;
                }
                if (!sendBody(c, again)) return;
            }
            synchronized (lock) {
                if (state != STARTED) return;
                status = Status.WAITING_FOR_RESPONSE;
            }
            int code = c.getResponseCode();
            connecting = false;
            ArrayList<Map.Entry<String, String>> list = new ArrayList<>();
            String protocol = "http/1.1";
            long bytes = 0;
            for (int i = 0; i < 1000; i++) {
                String k = c.getHeaderFieldKey(i), v = c.getHeaderField(i);
                if (k == null && v == null) break;
                if (k == null) continue;   // a status line, where a client lists it as a field
                bytes += k.length() + (v == null ? 0 : v.length()) + 4;
                if (k.regionMatches(true, 0, "X-Android-", 0, 10)) {
                    if ("X-Android-Selected-Protocol".equalsIgnoreCase(k) && v != null) protocol = v;
                    continue;
                }
                list.add(Info.entry(k, v == null ? "" : v));
            }
            String text = c.getResponseMessage();
            String location = code >= 300 && code < 400 ? c.getHeaderField("Location") : null;
            if (location != null) {
                String next = new URL(u, location).toString();
                final Info i;
                synchronized (lock) {
                    if (state != STARTED) return;
                    i = info = new Info(chain, code, text, list, protocol, bytes);
                }
                c.disconnect();
                if (++redirects > MAX_REDIRECTS) {
                    fail(new Failures.Network("Exception in UrlRequest: net::ERR_TOO_MANY_REDIRECTS", null, NetworkException.ERROR_OTHER));
                    return;
                }
                synchronized (lock) {
                    if (state != STARTED) return;
                    pendingRedirect = next;
                    redirectCode = code;
                    state = REDIRECT_RECEIVED;
                    status = Status.IDLE;
                }
                post(() -> {
                    synchronized (lock) {
                        if (state != REDIRECT_RECEIVED) return;
                        state = AWAITING_FOLLOW_REDIRECT;
                    }
                    try {
                        callback.onRedirectReceived(this, i, next);
                    } catch (Exception e) {
                        callbackFailed(e);
                    }
                });
                return;
            }
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            final Info i;
            synchronized (lock) {
                if (state != STARTED) {
                    closeQuietly(in);
                    return;
                }
                body = in;
                i = info = new Info(chain, code, text, list, protocol, bytes);
            }
            closeUpload();
            post(() -> {
                synchronized (lock) {
                    if (state != STARTED) return;
                    state = AWAITING_READ;
                    status = Status.IDLE;
                }
                try {
                    callback.onResponseStarted(this, i);
                } catch (Exception e) {
                    callbackFailed(e);
                }
            });
        } catch (UploadFailure e) {
            fail(new Failures.Callback("Exception received from UploadDataProvider", e.getCause()));
        } catch (IOException e) {
            networkFailed(e, target, connecting);
        } catch (RuntimeException e) {
            fail(Failures.network("UrlRequest", new IOException(e.toString(), e), connecting));
        }
    }

    private static String hostOf(String url) {
        try {
            return new URL(url).getHost();
        } catch (IOException e) {
            return "?";
        }
    }

    @Override
    public void followRedirect() {
        synchronized (lock) {
            if (state != AWAITING_FOLLOW_REDIRECT) throw new IllegalStateException("No redirect to follow.");
            if ((redirectCode == 303 && !"HEAD".equals(method)) || ((redirectCode == 301 || redirectCode == 302) && "POST".equals(method))) {
                method = "GET";
                sendBody = false;
            }
            url = pendingRedirect;
            pendingRedirect = null;
            chain.add(url);
            state = STARTED;
            status = Status.CONNECTING;
        }
        io(this::connect);
    }

    @Override
    public void read(ByteBuffer buffer) {
        if (!buffer.hasRemaining()) throw new IllegalArgumentException("ByteBuffer is already full.");
        synchronized (lock) {
            if (state != AWAITING_READ) throw new IllegalStateException("Unexpected read attempt.");
            state = READING;
            status = Status.READING_RESPONSE;
        }
        io(() -> read2(buffer));
    }

    private void read2(ByteBuffer buffer) {
        InputStream in;
        synchronized (lock) {
            if (state != READING) return;
            in = body;
        }
        int n;
        try {
            if (in == null) {
                n = -1;
            } else if (buffer.hasArray()) {
                n = in.read(buffer.array(), buffer.arrayOffset() + buffer.position(), buffer.remaining());
                if (n > 0) buffer.position(buffer.position() + n);
            } else {
                if (scratch == null) scratch = new byte[32 * 1024];
                n = in.read(scratch, 0, Math.min(scratch.length, buffer.remaining()));
                if (n > 0) buffer.put(scratch, 0, n);
            }
        } catch (IOException e) {
            networkFailed(e, url, false);
            return;
        }
        if (n < 0) {
            closeQuietly(in);
            final Info i;
            synchronized (lock) {
                if (state != READING) return;
                state = COMPLETE;
                status = Status.INVALID;
                i = info;
            }
            ended();
            post(() -> {
                try {
                    callback.onSucceeded(this, i);
                } catch (Exception e) {
                    android.util.Log.e("Megler", "HttpEngine: an exception in onSucceeded", e);
                }
            });
            return;
        }
        final Info i = info;
        if (n > 0) i.addReceived(n);
        post(() -> {
            synchronized (lock) {
                if (state != READING) return;
                state = AWAITING_READ;
                status = Status.IDLE;
            }
            try {
                callback.onReadCompleted(this, i, buffer);
            } catch (Exception e) {
                callbackFailed(e);
            }
        });
    }

    @Override
    public void cancel() {
        HttpURLConnection c;
        final Info i;
        synchronized (lock) {
            if (state == NOT_STARTED || state >= COMPLETE) return;
            state = CANCELLED;
            status = Status.INVALID;
            c = conn;
            i = info;
            lock.notifyAll();
        }
        disconnect(c);
        ended();
        post(() -> {
            try {
                callback.onCanceled(this, i);
            } catch (Exception e) {
                android.util.Log.e("Megler", "HttpEngine: an exception in onCanceled", e);
            }
        });
    }

    /** What the network did to a request still running (one the app canceled meets its closed connection): said in the
     *  log, and the request fails with it. */
    private void networkFailed(IOException e, String target, boolean connecting) {
        synchronized (lock) {
            if (state == NOT_STARTED || state >= COMPLETE) return;
        }
        Engine.sayFailure("a request", hostOf(target), e);
        fail(Failures.network("UrlRequest", e, connecting));
    }

    private void fail(final HttpException e) {
        HttpURLConnection c;
        final Info i;
        synchronized (lock) {
            if (state == NOT_STARTED || state >= COMPLETE) return;
            state = ERROR;
            status = Status.INVALID;
            c = conn;
            i = info;
            lock.notifyAll();
        }
        disconnect(c);
        ended();
        post(() -> {
            try {
                callback.onFailed(this, i, e);
            } catch (Exception x) {
                android.util.Log.e("Megler", "HttpEngine: an exception in onFailed", x);
            }
        });
    }

    private void callbackFailed(Exception e) {
        android.util.Log.e("Megler", "HttpEngine: the app's UrlRequest.Callback threw", e);
        fail(new Failures.Callback("Exception received from UrlRequest.Callback", e));
    }

    /** The request has ended, whichever way: the engine counts it no more, and the upload is closed. */
    private void ended() {
        engine.finished();
        closeUpload();
    }

    /** A connection given up from the app's thread: closed on one of the engine's (it may wait on the network). */
    private void disconnect(final HttpURLConnection c) {
        if (c == null) return;
        try {
            engine.io().execute(c::disconnect);
        } catch (RejectedExecutionException e) {
            c.disconnect();
        }
    }

    /** A callback onto the app's executor. One it refuses ends the request, which can tell the app nothing more. */
    private void post(Runnable r) {
        try {
            executor.execute(r);
        } catch (RejectedExecutionException e) {
            android.util.Log.e("Megler", "HttpEngine: the request's executor refused a callback", e);
            boolean end = false;
            HttpURLConnection c = null;
            synchronized (lock) {
                if (state != NOT_STARTED && state < COMPLETE) {
                    state = ERROR;
                    c = conn;
                    end = true;
                    lock.notifyAll();
                }
            }
            if (end) {
                disconnect(c);
                ended();
            }
        }
    }

    private static void closeQuietly(InputStream in) {
        if (in == null) return;
        try {
            in.close();
        } catch (IOException ignored) {
        }
    }

    // ---- the upload

    /** The app's provider failed: its exception is the cause. */
    private static final class UploadFailure extends IOException {
        UploadFailure(Throwable cause) { super(String.valueOf(cause), cause); }
    }

    /** What the provider answers through its sink, waited for on the engine's thread. */
    private final class UploadSink extends UploadDataSink {
        private int answered;   // 0 none, 1 read, 2 rewound
        private boolean last;
        private Exception error;

        @Override
        public void onReadSucceeded(boolean finalChunk) {
            synchronized (lock) {
                last = finalChunk;
                answered = 1;
                lock.notifyAll();
            }
        }

        @Override
        public void onReadError(Exception exception) {
            synchronized (lock) {
                error = exception != null ? exception : new IOException("onReadError(null)");
                answered = 1;
                lock.notifyAll();
            }
        }

        @Override
        public void onRewindSucceeded() {
            synchronized (lock) {
                answered = 2;
                lock.notifyAll();
            }
        }

        @Override
        public void onRewindError(Exception exception) {
            synchronized (lock) {
                error = exception != null ? exception : new IOException("onRewindError(null)");
                answered = 2;
                lock.notifyAll();
            }
        }

        /** Runs `ask` on the upload's executor and waits for the sink's answer; false if the request ended meanwhile. */
        boolean ask(Runnable ask) throws UploadFailure {
            synchronized (lock) {
                answered = 0;
                error = null;
            }
            try {
                uploadExecutor.execute(() -> {
                    try {
                        ask.run();
                    } catch (RuntimeException e) {
                        onReadError(e);
                    }
                });
            } catch (RejectedExecutionException e) {
                throw new UploadFailure(e);
            }
            synchronized (lock) {
                while (answered == 0 && state == STARTED) {
                    try {
                        lock.wait();
                    } catch (InterruptedException e) {
                        throw new UploadFailure(e);
                    }
                }
                if (state != STARTED) return false;
                if (error != null) throw new UploadFailure(error);
                return true;
            }
        }
    }

    /** The body, from the provider: its length (fixed, or -1 for chunked), rewound if it was sent before (a 307 or 308).
     *  False if the request ended while waiting for the provider. */
    private boolean sendBody(HttpURLConnection c, boolean again) throws IOException {
        final long[] length = { -1 };
        if (!sink.ask(() -> {
            try {
                length[0] = upload.getLength();
                sink.onReadSucceeded(false);
            } catch (Exception e) {
                sink.onReadError(e);
            }
        })) return false;
        if (again && !sink.ask(() -> {
            try {
                upload.rewind(sink);
            } catch (Exception e) {
                sink.onRewindError(e);
            }
        })) return false;
        if (length[0] >= 0) c.setFixedLengthStreamingMode(length[0]);
        else c.setChunkedStreamingMode(0);
        c.setDoOutput(true);
        synchronized (lock) {
            bodySent = true;
        }
        OutputStream out = c.getOutputStream();
        final ByteBuffer buffer = ByteBuffer.allocateDirect((int) Math.max(1, Math.min(16 * 1024, length[0] < 0 ? 16 * 1024 : length[0])));
        byte[] copy = new byte[buffer.capacity()];
        long total = 0;
        while (length[0] < 0 || total < length[0]) {
            buffer.clear();
            if (!sink.ask(() -> {
                try {
                    upload.read(sink, buffer);
                } catch (Exception e) {
                    sink.onReadError(e);
                }
            })) return false;
            boolean finalChunk;
            synchronized (lock) {
                finalChunk = sink.last;
            }
            buffer.flip();
            int n = buffer.remaining();
            if (length[0] >= 0 && total + n > length[0]) {
                throw new UploadFailure(new IllegalArgumentException("Read upload data length " + (total + n) + " exceeds expected length " + length[0]));
            }
            if (finalChunk && length[0] >= 0) throw new UploadFailure(new IllegalArgumentException("Non-chunked upload can't have last chunk"));
            buffer.get(copy, 0, n);
            out.write(copy, 0, n);
            total += n;
            if (finalChunk) break;
        }
        out.close();
        return true;
    }

    private void closeUpload() {
        if (upload == null) return;
        synchronized (lock) {
            if (uploadClosed) return;
            uploadClosed = true;
        }
        try {
            uploadExecutor.execute(() -> {
                try {
                    upload.close();
                } catch (IOException e) {
                    android.util.Log.w("Megler", "HttpEngine: the upload's provider failed to close", e);
                }
            });
        } catch (RejectedExecutionException ignored) {
        }
    }

    // ---- what the request says of itself

    @Override
    public void getStatus(final StatusListener listener) {
        final int s;
        synchronized (lock) {
            s = status;
        }
        try {
            executor.execute(() -> listener.onStatus(s));
        } catch (RejectedExecutionException e) {
            android.util.Log.e("Megler", "HttpEngine: the request's executor refused a status", e);
        }
    }

    @Override
    public boolean isDone() {
        synchronized (lock) {
            return state >= COMPLETE;
        }
    }

    @Override public String getHttpMethod() { return initialMethod; }
    @Override public HeaderBlock getHeaders() { return new Info.Headers(headers); }
    @Override public int getPriority() { return priority; }
    @Override public boolean isCacheDisabled() { return cacheDisabled; }
    @Override public boolean isDirectExecutorAllowed() { return directAllowed; }
    @Override public boolean hasTrafficStatsTag() { return hasTag; }
    @Override public boolean hasTrafficStatsUid() { return hasUid; }

    @Override
    public int getTrafficStatsTag() {
        if (!hasTag) throw new IllegalStateException("TrafficStatsTag is not set");
        return tag;
    }

    @Override
    public int getTrafficStatsUid() {
        if (!hasUid) throw new IllegalStateException("TrafficStatsUid is not set");
        return uid;
    }
}
