// Megler: Base64OutputStream's and Base64InputStream's coder, a piece at a time, with android.util.Base64's flags as its
// encode and decode take them (the streams were stubs whose writes went nowhere: Sberbank compressed a request's body
// through one, package 576).
package android.util;

final class MeglerBase64Coder {
    private static final byte[] STANDARD = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    private static final byte[] URL_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    /* Base64.encode's lines: 76 characters each, then a line end, the last too. */
    private static final int LINE = 76;

    private final boolean encode, wrap, crlf, pad;
    private final byte[] alphabet;
    private int tail, tailLen, lineChars;        // encoding: up to two bytes not yet a group, characters on the line
    private int acc, accLen;                     // decoding: the sextets of the group under way

    MeglerBase64Coder(int flags, boolean encode) {
        this.encode = encode;
        wrap = (flags & Base64.NO_WRAP) == 0;
        crlf = (flags & Base64.CRLF) != 0;
        pad = (flags & Base64.NO_PADDING) == 0;
        alphabet = (flags & Base64.URL_SAFE) != 0 ? URL_SAFE : STANDARD;
    }

    /* `len` bytes coded into `out`; with `finish` what is left of the last group too. */
    void process(byte[] in, int off, int len, boolean finish, java.io.ByteArrayOutputStream out) throws Base64DataException {
        if (encode) encode(in, off, len, finish, out);
        else decode(in, off, len, finish, out);
    }

    private void put(java.io.ByteArrayOutputStream out, int c) {
        if (wrap && lineChars == LINE) lineEnd(out);
        out.write(c);
        lineChars++;
    }

    private void lineEnd(java.io.ByteArrayOutputStream out) {
        if (crlf) out.write('\r');
        out.write('\n');
        lineChars = 0;
    }

    private void encode(byte[] in, int off, int len, boolean finish, java.io.ByteArrayOutputStream out) {
        for (int i = off; i < off + len; i++) {
            tail = (tail << 8) | (in[i] & 0xff);
            if (++tailLen == 3) {
                put(out, alphabet[(tail >> 18) & 63]);
                put(out, alphabet[(tail >> 12) & 63]);
                put(out, alphabet[(tail >> 6) & 63]);
                put(out, alphabet[tail & 63]);
                tail = tailLen = 0;
            }
        }
        if (!finish) return;
        if (tailLen == 1) {
            put(out, alphabet[(tail >> 2) & 63]);
            put(out, alphabet[(tail << 4) & 63]);
            if (pad) {
                put(out, '=');
                put(out, '=');
            }
        } else if (tailLen == 2) {
            put(out, alphabet[(tail >> 10) & 63]);
            put(out, alphabet[(tail >> 4) & 63]);
            put(out, alphabet[(tail << 2) & 63]);
            if (pad) put(out, '=');
        }
        tail = tailLen = 0;
        if (wrap && lineChars > 0) lineEnd(out);
    }

    /* Either alphabet; line ends, padding and anything else not of it skipped, as Base64.decode takes them. */
    private void decode(byte[] in, int off, int len, boolean finish, java.io.ByteArrayOutputStream out) throws Base64DataException {
        for (int i = off; i < off + len; i++) {
            int c = in[i] & 0xff, v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+' || c == '-') v = 62;
            else if (c == '/' || c == '_') v = 63;
            else continue;
            acc = (acc << 6) | v;
            if (++accLen == 4) {
                out.write((acc >> 16) & 0xff);
                out.write((acc >> 8) & 0xff);
                out.write(acc & 0xff);
                acc = accLen = 0;
            }
        }
        if (!finish) return;
        int left = accLen;
        if (left == 2) out.write((acc >> 4) & 0xff);
        else if (left == 3) {
            out.write((acc >> 10) & 0xff);
            out.write((acc >> 2) & 0xff);
        }
        acc = accLen = 0;
        if (left == 1) throw new Base64DataException("bad base-64");
    }
}
