// Megler: Android's Base64OutputStream, which encodes what is written to it (or decodes, `encode` false) into the stream it
// wraps, a piece at a time (MeglerBase64Coder). It was a stub whose writes went nowhere.
package android.util;

import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.OutputStream;

public class Base64OutputStream extends FilterOutputStream {
    private final MeglerBase64Coder coder;
    private final int flags;
    private final java.io.ByteArrayOutputStream coded = new java.io.ByteArrayOutputStream();
    private final byte[] single = new byte[1024];
    private int singles;
    private boolean closed;

    public Base64OutputStream(OutputStream out, int flags) {
        this(out, flags, true);
    }

    public Base64OutputStream(OutputStream out, int flags, boolean encode) {
        super(out);
        this.flags = flags;
        coder = new MeglerBase64Coder(flags, encode);
    }

    @Override public void write(int b) throws IOException {
        if (singles == single.length) flushSingles();
        single[singles++] = (byte) b;
    }

    @Override public void write(byte[] b, int off, int len) throws IOException {
        if (len <= 0) return;
        flushSingles();
        coder.process(b, off, len, false, coded);
        drain();
    }

    private void flushSingles() throws IOException {
        if (singles == 0) return;
        coder.process(single, 0, singles, false, coded);
        singles = 0;
        drain();
    }

    private void drain() throws IOException {
        if (coded.size() == 0) return;
        coded.writeTo(out);
        coded.reset();
    }

    @Override public void close() throws IOException {
        if (closed) return;
        closed = true;
        IOException thrown = null;
        try {
            flushSingles();
            coder.process(single, 0, 0, true, coded);
            drain();
        } catch (IOException e) {
            thrown = e;
        }
        try {
            if ((flags & Base64.NO_CLOSE) == 0) out.close();
            else out.flush();
        } catch (IOException e) {
            if (thrown == null) thrown = e;
        }
        if (thrown != null) throw thrown;
    }
}
