package megler.http;

import android.net.http.BidirectionalStream;
import android.net.http.HeaderBlock;
import android.net.http.HttpException;
import android.net.http.NetworkException;
import java.io.IOException;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/**
 * A BidirectionalStream of Megler's HttpEngine: one HTTP/2 stream (megler.http.H2) to an https URL, with Cronet's states
 * and callbacks. start() connects (or takes a connection the engine has) and sends the request's headers on one of the
 * engine's threads, then onStreamReady; writes queue until flush() sends them, each said by onWriteCompleted, the one that
 * ends the stream with END_STREAM; the response's headers come by onResponseHeadersReceived as soon as they are there,
 * whatever has been written; each read() gives what has come of the body (onReadCompleted), and the end with a read of
 * nothing and endOfStream; the trailers (gRPC's status) by onResponseTrailersReceived before that end. onSucceeded once both
 * ends are done; onFailed with a NetworkException (the server's reset, the connection's end) or a CallbackException (what
 * the app's callback threw); onCanceled after cancel(), which resets the stream (RST_STREAM CANCEL). GET and HEAD send
 * no body, as Cronet's do.
 */
final class Stream extends BidirectionalStream {
    static final class Builder extends BidirectionalStream.Builder {
        private final Engine engine;
        private final String url;
        private final Executor executor;
        private final Callback callback;
        private String method = "POST";
        private final ArrayList<Map.Entry<String, String>> headers = new ArrayList<>();
        private int priority = STREAM_PRIORITY_MEDIUM, tag, uid;
        private boolean delay, hasTag, hasUid;

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
        public BidirectionalStream.Builder setHttpMethod(String method) {
            if (method == null) throw new NullPointerException("Method is required.");
            this.method = method;
            return this;
        }

        @Override
        public BidirectionalStream.Builder addHeader(String name, String value) {
            if (name == null) throw new NullPointerException("Invalid header name.");
            if (value == null) throw new NullPointerException("Invalid header value.");
            headers.add(Info.entry(name, value));
            return this;
        }

        @Override public BidirectionalStream.Builder setPriority(int priority) { this.priority = priority; return this; }
        @Override public BidirectionalStream.Builder setDelayRequestHeadersUntilFirstFlushEnabled(boolean delay) { this.delay = delay; return this; }
        @Override public BidirectionalStream.Builder setTrafficStatsTag(int tag) { hasTag = true; this.tag = tag; return this; }
        @Override public BidirectionalStream.Builder setTrafficStatsUid(int uid) { hasUid = true; this.uid = uid; return this; }

        @Override
        public BidirectionalStream build() {
            return new Stream(this);
        }
    }

    /** One write the app asked for: its buffer and whether it ends the request. */
    private static final class Write {
        final ByteBuffer buffer;
        final boolean last;
        Write(ByteBuffer buffer, boolean last) { this.buffer = buffer; this.last = last; }
    }

    private static final int NOT_STARTED = 0, STARTED = 1, WAITING_FOR_READ = 2, READING = 3, READING_DONE = 4;
    private static final int WAITING_FOR_FLUSH = 2, WRITING = 3, WRITTEN = 4;

    private final Engine engine;
    private final String url, method;
    private final Executor executor;
    private final Callback callback;
    private final List<Map.Entry<String, String>> headers;
    private final int priority, tag, uid;
    private final boolean delay, hasTag, hasUid;

    private final Object lock = new Object();
    private int readState = NOT_STARTED, writeState = NOT_STARTED;
    private boolean done, readDone, writeDone, lastWriteQueued, headersPosted;
    private final ArrayDeque<Write> pending = new ArrayDeque<>(), flushing = new ArrayDeque<>();
    private H2.Stream h2;
    private Info info;
    private String host = "?";
    private byte[] scratch;

    private Stream(Builder b) {
        engine = b.engine;
        url = b.url;
        method = b.method;
        executor = b.executor;
        callback = b.callback;
        headers = Collections.unmodifiableList(new ArrayList<>(b.headers));
        priority = b.priority;
        tag = b.tag;
        uid = b.uid;
        delay = b.delay;
        hasTag = b.hasTag;
        hasUid = b.hasUid;
    }

    private static boolean hasBody(String method) {
        return !"GET".equals(method) && !"HEAD".equals(method);
    }

    @Override
    public void start() {
        synchronized (lock) {
            if (readState != NOT_STARTED) throw new IllegalStateException("Stream is already started.");
            engine.started();
            readState = writeState = STARTED;
        }
        io(this::open);
    }

    private void io(Runnable r) {
        try {
            engine.io().execute(r);
        } catch (RejectedExecutionException e) {
            fail(Failures.network("BidirectionalStream", new IOException("the engine is shut down", e), false));
        }
    }

    /** Connects, sends the headers, says the stream is ready, and waits for the response's headers. */
    private void open() {
        final boolean body = hasBody(method);
        H2.Stream s;
        try {
            URL u = new URL(url);
            host = u.getHost();
            if (!"https".equals(u.getProtocol())) {
                fail(new Failures.Network("Exception in BidirectionalStream: net::ERR_DISALLOWED_URL_SCHEME (" + u.getProtocol()
                        + ": a bidirectional stream is HTTP/2, over TLS)", null, NetworkException.ERROR_OTHER));
                return;
            }
            int port = u.getPort() < 0 ? 443 : u.getPort();
            s = engine.connections.open(host, port, fields(u, port), !body);
        } catch (IOException e) {
            networkFailed(e, true);
            return;
        } catch (RuntimeException e) {
            /* What TLS's provider may throw unchecked: the stream fails, it does not wait for ever. */
            networkFailed(new IOException(e.toString(), e), true);
            return;
        }
        synchronized (lock) {
            if (done) {
                s.cancel();
                return;
            }
            h2 = s;
        }
        post(() -> {
            synchronized (lock) {
                if (done) return;
                readState = WAITING_FOR_READ;
                if (body) {
                    writeState = WAITING_FOR_FLUSH;
                } else {
                    writeState = WRITTEN;
                    writeDone = true;
                }
            }
            try {
                callback.onStreamReady(this);
            } catch (Exception e) {
                callbackFailed(e);
            }
        });
        List<String[]> response;
        try {
            response = s.awaitHeaders();
        } catch (IOException e) {
            networkFailed(e, false);
            return;
        }
        final Info i = new Info(Collections.singletonList(url), s.status, "", entries(response), "h2", s.receivedBytes);
        synchronized (lock) {
            if (done) return;
            info = i;
        }
        post(() -> {
            synchronized (lock) {
                if (done) return;
            }
            try {
                callback.onResponseHeadersReceived(this, i);
            } catch (Exception e) {
                callbackFailed(e);
            }
        });
        synchronized (lock) {
            headersPosted = true;
            lock.notifyAll();
        }
    }

    /** The request's header fields: the pseudo-headers, then the app's in lower case, those HTTP/2 has no place for left out. */
    private List<String[]> fields(URL u, int port) {
        String authority = u.getHost() + (port == 443 ? "" : ":" + port);
        String path = u.getFile().isEmpty() ? "/" : u.getFile();
        ArrayList<String[]> out = new ArrayList<>();
        boolean agent = false;
        for (Map.Entry<String, String> h : headers) {
            String name = h.getKey().toLowerCase(Locale.ROOT);
            if (name.equals("host")) {
                authority = h.getValue();
                continue;
            }
            if (name.equals("connection") || name.equals("keep-alive") || name.equals("proxy-connection") || name.equals("transfer-encoding")
                    || name.equals("upgrade") || (name.equals("te") && !"trailers".equalsIgnoreCase(h.getValue().trim()))) continue;
            if (name.equals("user-agent")) agent = true;
            out.add(new String[] { name, h.getValue() });
        }
        if (!agent) out.add(new String[] { "user-agent", engine.userAgent });
        out.add(0, new String[] { ":path", path });
        out.add(0, new String[] { ":scheme", "https" });
        out.add(0, new String[] { ":authority", authority });
        out.add(0, new String[] { ":method", method });
        return out;
    }

    /** The response's fields as headers: its pseudo-headers (:status) are not among them. */
    private static List<Map.Entry<String, String>> entries(List<String[]> fields) {
        ArrayList<Map.Entry<String, String>> out = new ArrayList<>(fields.size());
        for (String[] f : fields) if (!f[0].startsWith(":")) out.add(Info.entry(f[0], f[1]));
        return out;
    }

    @Override
    public void read(ByteBuffer buffer) {
        synchronized (lock) {
            if (!buffer.hasRemaining()) throw new IllegalArgumentException("ByteBuffer is already full.");
            if (readState != WAITING_FOR_READ) throw new IllegalStateException("Unexpected read attempt.");
            if (done) return;
            readState = READING;
        }
        io(() -> read2(buffer));
    }

    private void read2(final ByteBuffer buffer) {
        H2.Stream s;
        synchronized (lock) {
            if (done) return;
            s = h2;
        }
        int n;
        try {
            if (buffer.hasArray()) {
                n = s.read(buffer.array(), buffer.arrayOffset() + buffer.position(), buffer.remaining());
            } else {
                if (scratch == null) scratch = new byte[32 * 1024];
                n = s.read(scratch, 0, Math.min(scratch.length, buffer.remaining()));
            }
        } catch (IOException e) {
            networkFailed(e, false);
            return;
        }
        /* What is read came after the response's headers: their callback is posted first. */
        synchronized (lock) {
            while (!headersPosted && !done) {
                try {
                    lock.wait();
                } catch (InterruptedException e) {
                    break;
                }
            }
            if (done) return;
        }
        final Info i = info;
        if (n > 0) {
            if (buffer.hasArray()) buffer.position(buffer.position() + n);
            else buffer.put(scratch, 0, n);
            i.addReceived(n);
        }
        final boolean end = n < 0;
        List<String[]> trailers = end ? s.trailers() : null;
        if (trailers != null) {
            final HeaderBlock block = new Info.Headers(entries(trailers));
            post(() -> {
                synchronized (lock) {
                    if (done) return;
                }
                try {
                    callback.onResponseTrailersReceived(this, i, block);
                } catch (Exception e) {
                    callbackFailed(e);
                }
            });
        }
        post(() -> {
            synchronized (lock) {
                if (done) return;
                readState = end ? READING_DONE : WAITING_FOR_READ;
                if (end) readDone = true;
            }
            try {
                callback.onReadCompleted(this, i, buffer, end);
            } catch (Exception e) {
                callbackFailed(e);
                return;
            }
            if (end) maybeSucceed();
        });
    }

    @Override
    public void write(ByteBuffer buffer, boolean endOfStream) {
        synchronized (lock) {
            if (!buffer.hasRemaining() && !endOfStream) throw new IllegalArgumentException("Empty buffer before end of stream.");
            if (lastWriteQueued) throw new IllegalArgumentException("Write after writing end of stream.");
            if (done) return;
            pending.add(new Write(buffer, endOfStream));
            if (endOfStream) lastWriteQueued = true;
        }
    }

    @Override
    public void flush() {
        synchronized (lock) {
            if (done || (writeState != WAITING_FOR_FLUSH && writeState != WRITING) || pending.isEmpty()) return;
            flushing.addAll(pending);
            pending.clear();
            if (writeState == WRITING) return;
            writeState = WRITING;
        }
        io(this::writes);
    }

    /** The flushed writes, in order, as the windows let them go: each said when it is on its way. */
    private void writes() {
        while (true) {
            final Write w;
            H2.Stream s;
            synchronized (lock) {
                if (done) return;
                w = flushing.poll();
                if (w == null) {
                    if (writeState == WRITING) writeState = WAITING_FOR_FLUSH;
                    return;
                }
                s = h2;
            }
            ByteBuffer b = w.buffer;
            try {
                int n = b.remaining();
                if (b.hasArray()) {
                    s.write(b.array(), b.arrayOffset() + b.position(), n, w.last);
                } else {
                    byte[] bytes = new byte[n];
                    b.duplicate().get(bytes);
                    s.write(bytes, 0, n, w.last);
                }
                b.position(b.limit());
            } catch (IOException e) {
                networkFailed(e, false);
                return;
            }
            if (w.last) {
                synchronized (lock) {
                    writeState = WRITTEN;
                }
            }
            post(() -> {
                final Info i;
                synchronized (lock) {
                    if (done) return;
                    if (w.last) writeDone = true;
                    i = info;
                }
                try {
                    callback.onWriteCompleted(this, i, w.buffer, w.last);
                } catch (Exception e) {
                    callbackFailed(e);
                    return;
                }
                if (w.last) maybeSucceed();
            });
        }
    }

    /** Both ends done, their callbacks run: the stream has succeeded (on the app's executor, after the last of them). */
    private void maybeSucceed() {
        final Info i;
        synchronized (lock) {
            if (done || !readDone || !writeDone) return;
            done = true;
            i = info;
        }
        engine.finished();
        try {
            callback.onSucceeded(this, i);
        } catch (Exception e) {
            android.util.Log.e("Megler", "HttpEngine: an exception in onSucceeded", e);
        }
    }

    @Override
    public void cancel() {
        final H2.Stream s;
        final Info i;
        synchronized (lock) {
            if (done || readState == NOT_STARTED) return;
            done = true;
            s = h2;
            i = info;
            lock.notifyAll();
        }
        if (s != null) resetLater(s);
        engine.finished();
        post(() -> {
            try {
                callback.onCanceled(this, i);
            } catch (Exception e) {
                android.util.Log.e("Megler", "HttpEngine: an exception in onCanceled", e);
            }
        });
    }

    /** What the network did to a stream that is not done yet (one the app canceled meets its own reset): said in the log,
     *  and the stream fails with it. */
    private void networkFailed(IOException e, boolean connecting) {
        synchronized (lock) {
            if (done) return;
        }
        Engine.sayFailure("a stream", host, e);
        fail(Failures.network("BidirectionalStream", e, connecting));
    }

    private void fail(final HttpException e) {
        final H2.Stream s;
        final Info i;
        synchronized (lock) {
            if (done || readState == NOT_STARTED) return;
            done = true;
            s = h2;
            i = info;
            lock.notifyAll();
        }
        if (s != null) resetLater(s);
        engine.finished();
        post(() -> {
            try {
                callback.onFailed(this, i, e);
            } catch (Exception x) {
                android.util.Log.e("Megler", "HttpEngine: an exception in onFailed", x);
            }
        });
    }

    /** The H2 stream reset on one of the engine's threads: the app's thread may be the UI's, and the socket may wait. */
    private void resetLater(final H2.Stream s) {
        try {
            engine.io().execute(s::cancel);
        } catch (RejectedExecutionException e) {
            s.cancel();
        }
    }

    private void callbackFailed(Exception e) {
        android.util.Log.e("Megler", "HttpEngine: the app's BidirectionalStream.Callback threw", e);
        fail(new Failures.Callback("Exception received from BidirectionalStream.Callback", e));
    }

    private void post(Runnable r) {
        try {
            executor.execute(r);
        } catch (RejectedExecutionException e) {
            android.util.Log.e("Megler", "HttpEngine: the stream's executor refused a callback", e);
            H2.Stream s = null;
            boolean end = false;
            synchronized (lock) {
                if (!done && readState != NOT_STARTED) {
                    done = end = true;
                    s = h2;
                    lock.notifyAll();
                }
            }
            if (end) {
                if (s != null) resetLater(s);
                engine.finished();
            }
        }
    }

    @Override
    public boolean isDone() {
        synchronized (lock) {
            return done;
        }
    }

    @Override public String getHttpMethod() { return method; }
    @Override public HeaderBlock getHeaders() { return new Info.Headers(headers); }
    @Override public int getPriority() { return priority; }
    @Override public boolean isDelayRequestHeadersUntilFirstFlushEnabled() { return delay; }
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
