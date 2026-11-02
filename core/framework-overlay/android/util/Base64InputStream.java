// Megler: Android's Base64InputStream, which decodes what it reads from the stream it wraps (or encodes, `encode` true), a
// piece at a time (MeglerBase64Coder). It was a stub that read nothing.
package android.util;

import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Base64InputStream extends FilterInputStream {
    private final MeglerBase64Coder coder;
    private final byte[] raw = new byte[2048];
    private final java.io.ByteArrayOutputStream coded = new java.io.ByteArrayOutputStream();
    private byte[] ready = new byte[0];
    private int at;
    private boolean ended;

    public Base64InputStream(InputStream in, int flags) {
        this(in, flags, false);
    }

    public Base64InputStream(InputStream in, int flags, boolean encode) {
        super(in);
        coder = new MeglerBase64Coder(flags, encode);
    }

    /* What is coded and not read yet; false at the end of the stream. */
    private boolean fill() throws IOException {
        while (at >= ready.length) {
            if (ended) return false;
            int n = in.read(raw);
            if (n < 0) {
                ended = true;
                coder.process(raw, 0, 0, true, coded);
            } else {
                coder.process(raw, 0, n, false, coded);
            }
            ready = coded.toByteArray();
            coded.reset();
            at = 0;
        }
        return true;
    }

    @Override public int read() throws IOException {
        return fill() ? ready[at++] & 0xff : -1;
    }

    @Override public int read(byte[] b, int off, int len) throws IOException {
        if (len == 0) return 0;
        if (!fill()) return -1;
        int n = Math.min(len, ready.length - at);
        System.arraycopy(ready, at, b, off, n);
        at += n;
        return n;
    }

    @Override public long skip(long n) throws IOException {
        long done = 0;
        while (done < n && fill()) {
            int k = (int) Math.min(n - done, ready.length - at);
            at += k;
            done += k;
        }
        return done;
    }

    @Override public int available() {
        return ready.length - at;
    }

    @Override public boolean markSupported() {
        return false;
    }

    @Override public void mark(int readlimit) {
        throw new UnsupportedOperationException();
    }

    @Override public void reset() {
        throw new UnsupportedOperationException();
    }

    @Override public void close() throws IOException {
        in.close();
        ready = new byte[0];
        at = 0;
    }
}
