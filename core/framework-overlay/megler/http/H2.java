package megler.http;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/**
 * HTTP/2 (RFC 7540) as a client, for Megler's HttpEngine: what a BidirectionalStream goes over (megler.http.Stream).
 *
 * A Connection is one TLS connection that negotiated "h2"; a thread of its own reads its frames, and each stream (a Stream
 * here) is a request and its response, with the request's DATA written by the thread that has them and the response's
 * headers, DATA and trailers kept for the thread that reads them. Flow control both ways: what a stream may send waits for
 * the server's windows; what it receives is granted back (WINDOW_UPDATE) as the app reads it, half a window at a time.
 * The server may not push (SETTINGS_ENABLE_PUSH 0), and a PUSH_PROMISE ends the connection.
 *
 * The response's first HEADERS are its headers (1xx ones are skipped), a later HEADERS with END_STREAM its trailers, kept
 * apart: gRPC's status is in the trailers, and Android 9's own HTTP/2 (okhttp 2.7's FramedStream) merges them into the
 * headers it has already handed over.
 *
 * One lock, the Connection's monitor, guards the state of the connection and of all its streams; the socket is written
 * under another (writeLock), taken first where both are, and never while waiting for the first.
 */
final class H2 {
    private H2() {}

    static final int DATA = 0, HEADERS = 1, PRIORITY = 2, RST_STREAM = 3, SETTINGS = 4, PUSH_PROMISE = 5, PING = 6, GOAWAY = 7,
            WINDOW_UPDATE = 8, CONTINUATION = 9;
    static final int END_STREAM = 0x1, ACK = 0x1, END_HEADERS = 0x4, PADDED = 0x8, PRIORITY_FLAG = 0x20;
    static final int NO_ERROR = 0, PROTOCOL_ERROR = 1, INTERNAL_ERROR = 2, FLOW_CONTROL_ERROR = 3, STREAM_CLOSED = 5,
            FRAME_SIZE_ERROR = 6, REFUSED_STREAM = 7, CANCEL = 8, COMPRESSION_ERROR = 9;
    private static final String[] ERRORS = { "NO_ERROR", "PROTOCOL_ERROR", "INTERNAL_ERROR", "FLOW_CONTROL_ERROR",
            "SETTINGS_TIMEOUT", "STREAM_CLOSED", "FRAME_SIZE_ERROR", "REFUSED_STREAM", "CANCEL", "COMPRESSION_ERROR",
            "CONNECT_ERROR", "ENHANCE_YOUR_CALM", "INADEQUATE_SECURITY", "HTTP_1_1_REQUIRED" };

    private static final byte[] PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n".getBytes(StandardCharsets.US_ASCII);
    /** What this side lets a stream and the whole connection receive ahead of the app's reads. */
    static final int STREAM_WINDOW = 1 << 20, CONNECTION_WINDOW = 16 << 20;
    /** The largest frame this side takes (the protocol's default; this side never raises it). */
    private static final int MAX_FRAME = 16384;
    /** Header blocks larger than this end the connection. */
    private static final int MAX_HEADER_BLOCK = 256 << 10;
    private static final int DEFAULT_WINDOW = 65535;

    static String errorName(int code) {
        return code >= 0 && code < ERRORS.length ? ERRORS[code] : "error 0x" + Integer.toHexString(code);
    }

    /** The server reset a stream (RST_STREAM), with its error code. */
    static final class Reset extends IOException {
        final int code;
        Reset(int code, String message) { super(message); this.code = code; }
    }

    /** The connection ended, from either side or by a broken socket: the reason, and the code a GOAWAY gave, if any. */
    static final class Ended extends IOException {
        final int code;
        final boolean refused;   // the server said (GOAWAY) it never took this stream: it may be made again elsewhere
        Ended(String message, int code, boolean refused, Throwable cause) {
            super(message, cause);
            this.code = code;
            this.refused = refused;
        }
    }

    /** The connection cannot take another stream (going away, closed, out of stream ids): another one is to be used. */
    static final class Unusable extends IOException {
        Unusable(String message) { super(message); }
    }

    /** One of the server's breaches of the protocol: the connection ends with a GOAWAY of this code. */
    private static final class Breach extends IOException {
        final int code;
        Breach(int code, String message) { super(message); this.code = code; }
    }

    static final class Connection implements Runnable {
        final String host;
        final int port;
        private final Socket socket;
        private final DataInputStream in;
        private final OutputStream out;
        private final Object writeLock = new Object();
        private final Hpack.Decoder decoder = new Hpack.Decoder(4096);
        private final HashMap<Integer, Stream> streams = new HashMap<>();
        private int nextId = 1;
        private boolean goingAway;
        private Ended ended;
        private int peerMaxStreams = Integer.MAX_VALUE, peerWindow = DEFAULT_WINDOW, peerMaxFrame = 16384;
        private long sendWindow = DEFAULT_WINDOW;
        private int connectionUnacked;
        private long receiveLeft = CONNECTION_WINDOW;
        private volatile long lastReadNs = System.nanoTime(), idleSinceNs = System.nanoTime();
        private long pingSentNs, pingAckedNs;

        Connection(String host, int port, Socket socket) throws IOException {
            this.host = host;
            this.port = port;
            this.socket = socket;
            this.in = new DataInputStream(new BufferedInputStream(socket.getInputStream(), 32 * 1024));
            this.out = new BufferedOutputStream(socket.getOutputStream(), MAX_FRAME + 9);
        }

        /** The preface and this side's settings: no push, a larger window for each stream, and for the connection. */
        void start() throws IOException {
            synchronized (writeLock) {
                out.write(PREFACE);
                header(12, SETTINGS, 0, 0);
                setting(2, 0);                     // SETTINGS_ENABLE_PUSH
                setting(4, STREAM_WINDOW);         // SETTINGS_INITIAL_WINDOW_SIZE
                windowUpdate(0, CONNECTION_WINDOW - DEFAULT_WINDOW);
                out.flush();
            }
            Thread reader = new Thread(this, "MeglerHttp2 " + host + ":" + port);
            reader.setDaemon(true);
            reader.start();
        }

        // ---- state seen by the pool

        synchronized boolean canTakeStream() {
            return ended == null && !goingAway && streams.size() < peerMaxStreams && nextId < 0x7ffffff0;
        }

        synchronized boolean isClosed() {
            return ended != null;
        }

        synchronized int activeStreams() {
            return streams.size();
        }

        /** Nanoseconds since the last stream ended, or 0 while one is open. */
        synchronized long idleNs() {
            return streams.isEmpty() ? System.nanoTime() - idleSinceNs : 0;
        }

        long quietNs() {
            return System.nanoTime() - lastReadNs;
        }

        /** Asks the server for a PING's answer (pingAnswered says when it came), unless one asked is still unanswered:
         *  false then, and nothing sent. */
        boolean ping() {
            synchronized (this) {
                if (pingSentNs != 0 && pingAckedNs - pingSentNs < 0) return false;
                pingSentNs = System.nanoTime();
            }
            try {
                synchronized (writeLock) {
                    header(8, PING, 0, 0);
                    out.write(new byte[8]);
                    out.flush();
                }
            } catch (IOException e) {
                close(new Ended("the connection broke: " + e.getMessage(), -1, false, e));
            }
            return true;
        }

        synchronized boolean pingAnswered() {
            return pingAckedNs != 0 && pingAckedNs - pingSentNs >= 0;
        }

        // ---- streams

        /** A new stream: its HEADERS (and CONTINUATIONs) sent, with END_STREAM when the request has no body. */
        Stream newStream(List<String[]> fields, boolean endStream) throws IOException {
            byte[] block = Hpack.encode(fields);
            synchronized (writeLock) {
                Stream s;
                synchronized (this) {
                    if (ended != null) throw new Unusable("the connection to " + host + " has ended");
                    if (goingAway || nextId >= 0x7ffffff0) throw new Unusable("the connection to " + host + " is going away");
                    if (streams.size() >= peerMaxStreams) throw new Unusable(host + " takes no more streams on this connection");
                    s = new Stream(this, nextId, peerWindow);
                    nextId += 2;
                    s.localClosed = endStream;
                    s.sentHeaderBytes = block.length;
                    streams.put(s.id, s);
                }
                try {
                    int first = Math.min(block.length, peerMaxFrame());
                    header(first, HEADERS, (endStream ? END_STREAM : 0) | (first == block.length ? END_HEADERS : 0), s.id);
                    out.write(block, 0, first);
                    for (int at = first; at < block.length; ) {
                        int n = Math.min(block.length - at, peerMaxFrame());
                        header(n, CONTINUATION, at + n == block.length ? END_HEADERS : 0, s.id);
                        out.write(block, at, n);
                        at += n;
                    }
                    out.flush();
                } catch (IOException e) {
                    Ended why = new Ended("the connection broke: " + e.getMessage(), -1, false, e);
                    close(why);
                    throw why;
                }
                return s;
            }
        }

        private synchronized int peerMaxFrame() {
            return peerMaxFrame;
        }

        /** Ends the connection: every stream fails with `why`, the socket is closed. */
        void close(Ended why) {
            synchronized (this) {
                if (ended != null) return;
                ended = why;
                for (Stream s : new ArrayList<>(streams.values())) s.failLocked(why);
                streams.clear();
                idleSinceNs = System.nanoTime();
                notifyAll();
            }
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }

        /** The engine's shutdown, or an idle connection let go: a GOAWAY, then the socket closed. */
        void shutdown() {
            try {
                synchronized (writeLock) {
                    header(8, GOAWAY, 0, 0);
                    out.write(new byte[8]);   // last stream 0 (none of the server's taken), NO_ERROR
                    out.flush();
                }
            } catch (IOException ignored) {
            }
            close(new Ended("the connection to " + host + " was closed by this side", NO_ERROR, false, null));
        }

        // ---- frames out (writeLock held by the caller, or taken here)

        private void header(int length, int type, int flags, int id) throws IOException {
            out.write(length >>> 16);
            out.write(length >>> 8);
            out.write(length);
            out.write(type);
            out.write(flags);
            writeInt(id & 0x7fffffff);
        }

        private void writeInt(int v) throws IOException {
            out.write(v >>> 24);
            out.write(v >>> 16);
            out.write(v >>> 8);
            out.write(v);
        }

        private void setting(int id, int value) throws IOException {
            out.write(id >>> 8);
            out.write(id);
            writeInt(value);
        }

        private void windowUpdate(int id, int increment) throws IOException {
            header(4, WINDOW_UPDATE, 0, id);
            writeInt(increment);
        }

        void writeData(int id, byte[] b, int off, int len, boolean fin) throws IOException {
            synchronized (writeLock) {
                try {
                    header(len, DATA, fin ? END_STREAM : 0, id);
                    out.write(b, off, len);
                    out.flush();
                } catch (IOException e) {
                    Ended why = new Ended("the connection broke: " + e.getMessage(), -1, false, e);
                    close(why);
                    throw why;
                }
            }
        }

        void writeReset(int id, int code) {
            try {
                synchronized (writeLock) {
                    header(4, RST_STREAM, 0, id);
                    writeInt(code);
                    out.flush();
                }
            } catch (IOException e) {
                close(new Ended("the connection broke: " + e.getMessage(), -1, false, e));
            }
        }

        /** The windows granted back: the stream's (0 for none) and the connection's. */
        void grant(int id, int streamIncrement, int connectionIncrement) {
            if (streamIncrement <= 0 && connectionIncrement <= 0) return;
            try {
                synchronized (writeLock) {
                    if (streamIncrement > 0) windowUpdate(id, streamIncrement);
                    if (connectionIncrement > 0) windowUpdate(0, connectionIncrement);
                    out.flush();
                }
            } catch (IOException e) {
                close(new Ended("the connection broke: " + e.getMessage(), -1, false, e));
            }
        }

        /** Bytes of the connection's window used by data no stream will read (padding, a closed stream's): granted back. */
        private int consumedLocked(int bytes) {
            connectionUnacked += bytes;
            if (connectionUnacked < CONNECTION_WINDOW / 2) return 0;
            int n = connectionUnacked;
            connectionUnacked = 0;
            receiveLeft += n;
            return n;
        }

        // ---- frames in

        @Override
        public void run() {
            Ended why;
            try {
                while (true) readFrame();
            } catch (Breach e) {
                try {
                    synchronized (writeLock) {
                        header(8, GOAWAY, 0, 0);
                        writeInt(0);
                        writeInt(e.code);
                        out.flush();
                    }
                } catch (IOException ignored) {
                }
                why = new Ended(host + " broke HTTP/2 (" + errorName(e.code) + "): " + e.getMessage(), e.code, false, e);
            } catch (EOFException e) {
                why = new Ended(host + " closed the connection", -1, false, e);
            } catch (IOException e) {
                boolean ours;
                synchronized (this) {
                    ours = ended != null;
                }
                why = ours ? null : new Ended("the connection to " + host + " broke: " + e.getMessage(), -1, false, e);
            } catch (RuntimeException e) {
                why = new Ended("the connection to " + host + " failed: " + e, INTERNAL_ERROR, false, e);
            }
            if (why != null) close(why);
        }

        private void readFrame() throws IOException {
            int length = in.readUnsignedByte() << 16 | in.readUnsignedByte() << 8 | in.readUnsignedByte();
            int type = in.readUnsignedByte(), flags = in.readUnsignedByte(), id = in.readInt() & 0x7fffffff;
            lastReadNs = System.nanoTime();
            if (length > MAX_FRAME) throw new Breach(FRAME_SIZE_ERROR, "a frame of " + length + " bytes");
            switch (type) {
                case DATA: data(length, flags, id); break;
                case HEADERS: headers(length, flags, id); break;
                case RST_STREAM: reset(length, id); break;
                case SETTINGS: settings(length, flags, id); break;
                case PUSH_PROMISE: throw new Breach(PROTOCOL_ERROR, "a PUSH_PROMISE, which this side's settings refuse");
                case PING: ping(length, flags, id); break;
                case GOAWAY: goAway(length, id); break;
                case WINDOW_UPDATE: readWindowUpdate(length, id); break;
                case CONTINUATION: throw new Breach(PROTOCOL_ERROR, "a CONTINUATION that follows no HEADERS");
                default: in.skipBytes(length);   // PRIORITY, and frames of types this side does not know
            }
        }

        private void data(int length, int flags, int id) throws IOException {
            if (id == 0) throw new Breach(PROTOCOL_ERROR, "DATA on stream 0");
            int pad = 0, n = length;
            if ((flags & PADDED) != 0) {
                if (n < 1) throw new Breach(PROTOCOL_ERROR, "padded DATA without its pad length");
                pad = in.readUnsignedByte();
                n -= 1 + pad;
                if (n < 0) throw new Breach(PROTOCOL_ERROR, "DATA padded past its end");
            }
            byte[] bytes = new byte[n];
            in.readFully(bytes);
            in.skipBytes(pad);
            int grantConnection = 0, resetCode = -1;
            synchronized (this) {
                receiveLeft -= length;
                if (receiveLeft < 0) throw new Breach(FLOW_CONTROL_ERROR, "DATA past the connection's window");
                Stream s = streams.get(id);
                if (s == null || s.remoteClosed || s.failure != null) {
                    grantConnection = consumedLocked(length);
                } else if (!s.headersReceived) {
                    resetCode = PROTOCOL_ERROR;
                    s.failLocked(new Reset(PROTOCOL_ERROR, host + " sent DATA before the response's headers"));
                    grantConnection = consumedLocked(length);
                } else {
                    s.receiveLeft -= length;
                    if (s.receiveLeft < 0) {
                        resetCode = FLOW_CONTROL_ERROR;
                        s.failLocked(new Reset(FLOW_CONTROL_ERROR, host + " sent DATA past the stream's window"));
                        grantConnection = consumedLocked(length);
                    } else {
                        if (n > 0) s.data.add(bytes);
                        s.buffered += n;
                        s.receivedBytes += length;
                        s.paddingUnacked += length - n;
                        if ((flags & END_STREAM) != 0) s.remoteClosed = true;
                        s.closeIfDoneLocked();
                    }
                }
                notifyAll();
            }
            if (resetCode >= 0) writeReset(id, resetCode);
            if (grantConnection > 0) grant(0, 0, grantConnection);
        }

        private void headers(int length, int flags, int id) throws IOException {
            if (id == 0) throw new Breach(PROTOCOL_ERROR, "HEADERS on stream 0");
            int pad = 0, n = length;
            if ((flags & PADDED) != 0) {
                if (n < 1) throw new Breach(PROTOCOL_ERROR, "padded HEADERS without its pad length");
                pad = in.readUnsignedByte();
                n--;
            }
            if ((flags & PRIORITY_FLAG) != 0) {
                if (n < 5) throw new Breach(PROTOCOL_ERROR, "HEADERS with a priority too short");
                in.skipBytes(5);
                n -= 5;
            }
            n -= pad;
            if (n < 0) throw new Breach(PROTOCOL_ERROR, "HEADERS padded past its end");
            ByteArrayOutputStream block = new ByteArrayOutputStream(Math.max(n, 16));
            byte[] part = new byte[n];
            in.readFully(part);
            block.write(part, 0, n);
            in.skipBytes(pad);
            boolean done = (flags & END_HEADERS) != 0;
            int total = length;
            while (!done) {
                int l = in.readUnsignedByte() << 16 | in.readUnsignedByte() << 8 | in.readUnsignedByte();
                int t = in.readUnsignedByte(), f = in.readUnsignedByte(), i = in.readInt() & 0x7fffffff;
                if (t != CONTINUATION || i != id) throw new Breach(PROTOCOL_ERROR, "a header block cut by another frame");
                if (l > MAX_FRAME) throw new Breach(FRAME_SIZE_ERROR, "a frame of " + l + " bytes");
                total += l;
                if (total > MAX_HEADER_BLOCK) throw new Breach(PROTOCOL_ERROR, "a header block of more than " + MAX_HEADER_BLOCK + " bytes");
                part = new byte[l];
                in.readFully(part);
                block.write(part, 0, l);
                done = (f & END_HEADERS) != 0;
            }
            List<String[]> fields;
            try {
                fields = decoder.decode(block.toByteArray());   // also for a stream no longer here: the table is the connection's
            } catch (Hpack.Malformed e) {
                throw new Breach(COMPRESSION_ERROR, e.getMessage());
            }
            boolean fin = (flags & END_STREAM) != 0;
            int resetCode = -1;
            synchronized (this) {
                Stream s = streams.get(id);
                if (s != null && s.failure == null) {
                    s.receivedBytes += total;
                    if (!s.headersReceived) {
                        int status = status(fields);
                        if (status < 0) {
                            resetCode = PROTOCOL_ERROR;
                            s.failLocked(new Reset(PROTOCOL_ERROR, host + " answered without a :status"));
                        } else if (status >= 100 && status < 200 && status != 101) {
                            if (fin) {
                                resetCode = PROTOCOL_ERROR;
                                s.failLocked(new Reset(PROTOCOL_ERROR, host + " ended the stream on an informational response"));
                            }
                            /* else an informational response (100 Continue, 103 Early Hints): the real one follows. */
                        } else {
                            s.headers = fields;
                            s.status = status;
                            s.headersReceived = true;
                        }
                    } else if (!fin) {
                        resetCode = PROTOCOL_ERROR;
                        s.failLocked(new Reset(PROTOCOL_ERROR, host + " sent a second HEADERS that does not end the stream"));
                    } else {
                        s.trailers = fields;
                    }
                    if (fin && s.failure == null) s.remoteClosed = true;
                    s.closeIfDoneLocked();
                }
                notifyAll();
            }
            if (resetCode >= 0) writeReset(id, resetCode);
        }

        private static int status(List<String[]> fields) {
            for (String[] f : fields) {
                if (":status".equals(f[0])) {
                    try {
                        return Integer.parseInt(f[1].trim());
                    } catch (NumberFormatException e) {
                        return -1;
                    }
                }
            }
            return -1;
        }

        private void reset(int length, int id) throws IOException {
            if (length != 4 || id == 0) throw new Breach(PROTOCOL_ERROR, "a RST_STREAM of " + length + " bytes on stream " + id);
            int code = in.readInt();
            synchronized (this) {
                Stream s = streams.get(id);
                if (s != null) s.failLocked(new Reset(code, host + " reset the stream (" + errorName(code) + ")"));
                notifyAll();
            }
        }

        private void settings(int length, int flags, int id) throws IOException {
            if (id != 0) throw new Breach(PROTOCOL_ERROR, "SETTINGS on a stream");
            if ((flags & ACK) != 0) {
                if (length != 0) throw new Breach(FRAME_SIZE_ERROR, "a SETTINGS acknowledgement with a payload");
                return;
            }
            if (length % 6 != 0) throw new Breach(FRAME_SIZE_ERROR, "SETTINGS of " + length + " bytes");
            int[] ids = new int[length / 6], values = new int[length / 6];
            for (int i = 0; i < ids.length; i++) {
                ids[i] = in.readUnsignedShort();
                values[i] = in.readInt();
            }
            synchronized (this) {
                for (int i = 0; i < ids.length; i++) {
                    int value = values[i];
                    switch (ids[i]) {
                        case 3:   // SETTINGS_MAX_CONCURRENT_STREAMS
                            peerMaxStreams = value < 0 ? Integer.MAX_VALUE : value;
                            break;
                        case 4: {   // SETTINGS_INITIAL_WINDOW_SIZE: every stream's window moves by the difference
                            if (value < 0) throw new Breach(FLOW_CONTROL_ERROR, "an initial window of " + (value & 0xffffffffL));
                            long delta = (long) value - peerWindow;
                            peerWindow = value;
                            for (Stream s : streams.values()) s.sendWindow += delta;
                            break;
                        }
                        case 5:   // SETTINGS_MAX_FRAME_SIZE
                            if (value < 16384 || value > 16777215) throw new Breach(PROTOCOL_ERROR, "a largest frame of " + value);
                            peerMaxFrame = value;
                            break;
                        default:   // the header table (the encoder uses none), push (a server's), header lists, others
                            break;
                    }
                }
                notifyAll();
            }
            synchronized (writeLock) {
                header(0, SETTINGS, ACK, 0);
                out.flush();
            }
        }

        private void ping(int length, int flags, int id) throws IOException {
            if (length != 8 || id != 0) throw new Breach(PROTOCOL_ERROR, "a PING of " + length + " bytes on stream " + id);
            byte[] payload = new byte[8];
            in.readFully(payload);
            if ((flags & ACK) != 0) {
                synchronized (this) {
                    pingAckedNs = System.nanoTime();
                }
                return;
            }
            synchronized (writeLock) {
                header(8, PING, ACK, 0);
                out.write(payload);
                out.flush();
            }
        }

        private void goAway(int length, int id) throws IOException {
            if (length < 8 || id != 0) throw new Breach(PROTOCOL_ERROR, "a GOAWAY of " + length + " bytes on stream " + id);
            int last = in.readInt() & 0x7fffffff, code = in.readInt();
            byte[] debug = new byte[Math.min(length - 8, 256)];
            in.readFully(debug);
            in.skipBytes(length - 8 - debug.length);
            String said = debug.length > 0 ? ": " + new String(debug, StandardCharsets.UTF_8) : "";
            boolean empty;
            synchronized (this) {
                goingAway = true;
                for (Stream s : new ArrayList<>(streams.values())) {
                    if (s.id > last) s.failLocked(new Ended(host + " went away (" + errorName(code) + ") before it took the stream" + said, code, true, null));
                }
                empty = streams.isEmpty();
                notifyAll();
            }
            if (empty) close(new Ended(host + " went away (" + errorName(code) + ")" + said, code, false, null));
        }

        private void readWindowUpdate(int length, int id) throws IOException {
            if (length != 4) throw new Breach(FRAME_SIZE_ERROR, "a WINDOW_UPDATE of " + length + " bytes");
            int increment = in.readInt() & 0x7fffffff;
            int resetCode = -1;
            synchronized (this) {
                if (id == 0) {
                    if (increment == 0) throw new Breach(PROTOCOL_ERROR, "a WINDOW_UPDATE of 0 for the connection");
                    sendWindow += increment;
                    if (sendWindow > Integer.MAX_VALUE) throw new Breach(FLOW_CONTROL_ERROR, "the connection's window past 2^31 - 1");
                } else {
                    Stream s = streams.get(id);
                    if (s != null && s.failure == null) {
                        if (increment == 0) {
                            resetCode = PROTOCOL_ERROR;
                            s.failLocked(new Reset(PROTOCOL_ERROR, host + " sent a WINDOW_UPDATE of 0"));
                        } else {
                            s.sendWindow += increment;
                            if (s.sendWindow > Integer.MAX_VALUE) {
                                resetCode = FLOW_CONTROL_ERROR;
                                s.failLocked(new Reset(FLOW_CONTROL_ERROR, host + " gave the stream a window past 2^31 - 1"));
                            }
                        }
                    }
                }
                notifyAll();
            }
            if (resetCode >= 0) writeReset(id, resetCode);
        }
    }

    /** One request and its response on a Connection. Its state is guarded by the Connection's monitor. */
    static final class Stream {
        final Connection c;
        final int id;
        long sendWindow;
        int receiveLeft = STREAM_WINDOW;
        private int unacked, paddingUnacked;
        boolean localClosed, remoteClosed, headersReceived;
        List<String[]> headers, trailers;
        int status;
        private final ArrayDeque<byte[]> data = new ArrayDeque<>();
        private int offset, buffered;
        long receivedBytes, sentHeaderBytes;
        IOException failure;
        private boolean removed;

        Stream(Connection c, int id, long sendWindow) {
            this.c = c;
            this.id = id;
            this.sendWindow = sendWindow;
        }

        /**
         * Fails the stream (the connection's monitor held): its waiters wake, and it leaves the connection. A response the
         * server has already ended stays readable to its end (a RST_STREAM of NO_ERROR after it only stops the request's
         * body); the rest of one that has not is dropped, and the connection's window gets its bytes back.
         */
        void failLocked(IOException why) {
            if (failure == null) {
                failure = why;
                if (!remoteClosed) {
                    int left = buffered - offset + paddingUnacked;
                    data.clear();
                    buffered = offset = paddingUnacked = 0;
                    giveBackLocked(left);
                }
            }
            removeLocked();
        }

        /** A stream both of whose ends are closed leaves the connection. */
        void closeIfDoneLocked() {
            if (localClosed && remoteClosed) removeLocked();
        }

        private void removeLocked() {
            if (removed) return;
            removed = true;
            c.streams.remove(id);
            if (c.streams.isEmpty()) c.idleSinceNs = System.nanoTime();
            if (c.goingAway && c.streams.isEmpty() && c.ended == null) {
                new Thread(() -> c.close(new Ended(c.host + " went away", NO_ERROR, false, null)), "MeglerHttp2 close").start();
            }
        }

        /** Bytes of the connection's window no read will take: given back, on a thread of its own when that reaches a grant
         *  (the socket is not written with the connection's monitor held). */
        private void giveBackLocked(int bytes) {
            if (bytes <= 0 || c.ended != null) return;
            final int n = c.consumedLocked(bytes);
            if (n > 0) new Thread(() -> c.grant(0, 0, n), "MeglerHttp2 grant").start();
        }

        /** The response's headers, waited for; the stream's failure if it fails first. */
        List<String[]> awaitHeaders() throws IOException {
            synchronized (c) {
                while (!headersReceived && failure == null) waitOn(c);
                if (!headersReceived) throw failure;
                return headers;
            }
        }

        /** Up to `len` bytes of the response's body, waited for; -1 at its end; the failure if the stream fails first. */
        int read(byte[] b, int off, int len) throws IOException {
            int n = 0, grantStream = 0, grantConnection;
            synchronized (c) {
                while (data.isEmpty() && !remoteClosed && failure == null) waitOn(c);
                if (data.isEmpty()) {
                    if (remoteClosed) return -1;
                    throw failure;
                }
                while (n < len && !data.isEmpty()) {
                    byte[] first = data.peekFirst();
                    int k = Math.min(len - n, first.length - offset);
                    System.arraycopy(first, offset, b, off + n, k);
                    n += k;
                    offset += k;
                    if (offset == first.length) {
                        data.removeFirst();
                        buffered -= first.length;
                        offset = 0;
                    }
                }
                int padding = paddingUnacked;
                paddingUnacked = 0;
                unacked += n + padding;
                if (!remoteClosed && unacked >= STREAM_WINDOW / 2) {
                    grantStream = unacked;
                    receiveLeft += unacked;
                    unacked = 0;
                }
                grantConnection = c.consumedLocked(n + padding);
            }
            c.grant(id, grantStream, grantConnection);
            return n;
        }

        /** The trailers, once read() has said the end; null when the server sent none. */
        List<String[]> trailers() {
            synchronized (c) {
                return trailers;
            }
        }

        /** Writes the body's bytes as DATA frames as the windows allow, the last with END_STREAM when `fin`. */
        void write(byte[] b, int off, int len, boolean fin) throws IOException {
            if (len == 0 && !fin) return;
            do {
                int chunk;
                boolean last;
                synchronized (c) {
                    while (failure == null && len > 0 && (sendWindow <= 0 || c.sendWindow <= 0)) waitOn(c);
                    if (failure instanceof Reset && ((Reset) failure).code == NO_ERROR && remoteClosed) {
                        /* The server has answered whole and asks for no more of the request (RFC 7540, 8.1): what is
                           left of it is not sent, and the request counts as ended. */
                        localClosed = true;
                        return;
                    }
                    if (failure != null) throw failure;
                    if (localClosed) throw new IOException("the stream's request has already ended");
                    chunk = len == 0 ? 0 : (int) Math.min(len, Math.min(Math.min(sendWindow, c.sendWindow), c.peerMaxFrame));
                    sendWindow -= chunk;
                    c.sendWindow -= chunk;
                    last = fin && chunk == len;
                    if (last) {
                        localClosed = true;
                        closeIfDoneLocked();
                    }
                }
                c.writeData(id, b, off, chunk, last);
                off += chunk;
                len -= chunk;
            } while (len > 0);
        }

        /** This side gives the stream up (RST_STREAM CANCEL), unless it has ended already. */
        void cancel() {
            boolean send;
            synchronized (c) {
                send = !removed && c.ended == null;
                failLocked(new IOException("the stream was canceled"));
                c.notifyAll();
            }
            if (send) c.writeReset(id, CANCEL);
        }

        private static void waitOn(Object monitor) throws InterruptedIOException {
            try {
                monitor.wait();
            } catch (InterruptedException e) {
                throw new InterruptedIOException("interrupted while waiting for the stream");
            }
        }
    }
}
