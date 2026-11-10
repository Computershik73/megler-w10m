package megler.http;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * HPACK (RFC 7541), the header compression of HTTP/2, for Megler's HttpEngine (megler.http.H2).
 *
 * The decoder takes what a server sends: indexed fields of the static and the dynamic table, literals with and without
 * indexing and never indexed, size updates of the dynamic table, and strings in Huffman's code. The encoder writes every
 * field as a literal that is not indexed, and the sensitive ones (authorization, cookies) as never indexed, with plain
 * strings: the dynamic table of the server's decoder is never used, which HPACK allows, and nothing a request says is
 * remembered for another.
 *
 * The Huffman code is RFC 7541's (Appendix B), canonical: only the length of each symbol's code is written below, and the
 * codes follow from the lengths in the order of length and then symbol. The lengths are those of Android 9's own HTTP
 * client (okhttp's Huffman.CODE_LENGTHS), whose codes the canonical order gives back exactly; RFC 7541's examples of
 * C.4 and C.6 decode with them (the data app's step "http-engine-hpack").
 */
final class Hpack {
    private Hpack() {}

    /** The static table, from index 1 (RFC 7541, Appendix A). */
    private static final String[][] STATIC = {
        null,
        { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" }, { ":path", "/" }, { ":path", "/index.html" },
        { ":scheme", "http" }, { ":scheme", "https" }, { ":status", "200" }, { ":status", "204" }, { ":status", "206" },
        { ":status", "304" }, { ":status", "400" }, { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" },
        { "accept-encoding", "gzip, deflate" }, { "accept-language", "" }, { "accept-ranges", "" }, { "accept", "" },
        { "access-control-allow-origin", "" }, { "age", "" }, { "allow", "" }, { "authorization", "" }, { "cache-control", "" },
        { "content-disposition", "" }, { "content-encoding", "" }, { "content-language", "" }, { "content-length", "" },
        { "content-location", "" }, { "content-range", "" }, { "content-type", "" }, { "cookie", "" }, { "date", "" },
        { "etag", "" }, { "expect", "" }, { "expires", "" }, { "from", "" }, { "host", "" }, { "if-match", "" },
        { "if-modified-since", "" }, { "if-none-match", "" }, { "if-range", "" }, { "if-unmodified-since", "" },
        { "last-modified", "" }, { "link", "" }, { "location", "" }, { "max-forwards", "" }, { "proxy-authenticate", "" },
        { "proxy-authorization", "" }, { "range", "" }, { "referer", "" }, { "refresh", "" }, { "retry-after", "" },
        { "server", "" }, { "set-cookie", "" }, { "strict-transport-security", "" }, { "transfer-encoding", "" },
        { "user-agent", "" }, { "vary", "" }, { "via", "" }, { "www-authenticate", "" },
    };

    /** The length in bits of each symbol's code, 0 to 255 and EOS (256). */
    private static final byte[] LENGTHS = {
        13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
        6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
        13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
        15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5, 6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
        20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23, 24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
        22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23, 21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
        26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25, 19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
        20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23, 26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
        30,
    };

    /** The decoding tree: node n's children are TREE[2n] (bit 0) and TREE[2n + 1] (bit 1); a child >= 0 is a node, a leaf is
     *  -1 - symbol, and 0 is no child (node 0 is the root, which no node points at). */
    private static final int[] TREE;
    private static final int EOS = 256;

    static {
        int n = LENGTHS.length;
        Integer[] order = new Integer[n];
        for (int i = 0; i < n; i++) order[i] = i;
        java.util.Arrays.sort(order, (a, b) -> LENGTHS[a] != LENGTHS[b] ? LENGTHS[a] - LENGTHS[b] : a - b);
        int[] tree = new int[2 * 2 * n];
        int nodes = 1;
        long code = 0;
        int previous = LENGTHS[order[0]];
        for (int i = 0; i < n; i++) {
            int symbol = order[i], length = LENGTHS[symbol];
            if (i > 0) code = (code + 1) << (length - previous);
            previous = length;
            int node = 0;
            for (int bit = length - 1; bit >= 0; bit--) {
                int b = (int) (code >>> bit) & 1, at = 2 * node + b;
                if (bit == 0) {
                    tree[at] = -1 - symbol;
                } else {
                    if (tree[at] == 0) tree[at] = nodes++;
                    node = tree[at];
                }
            }
        }
        TREE = tree;
    }

    /** A header block that breaks HPACK's rules: the connection's error (COMPRESSION_ERROR). */
    static final class Malformed extends IOException {
        Malformed(String message) { super("HPACK: " + message); }
    }

    /** One connection's decoder: its dynamic table lasts as long as the connection. */
    static final class Decoder {
        private final ArrayList<String[]> dynamic = new ArrayList<>();   // the newest first
        private final int settingMax;
        private int max, size;
        private byte[] b;
        private int at;

        /** settingMax: the most this side allows the table (SETTINGS_HEADER_TABLE_SIZE, 4096 when not said). */
        Decoder(int settingMax) {
            this.settingMax = settingMax;
            this.max = settingMax;
        }

        /** The fields of one header block, in its order, each { name, value }. */
        List<String[]> decode(byte[] block) throws IOException {
            b = block;
            at = 0;
            ArrayList<String[]> out = new ArrayList<>();
            boolean fields = false;
            while (at < b.length) {
                int x = b[at] & 0xff;
                if ((x & 0x80) != 0) {
                    out.add(entry(integer(7)));
                    fields = true;
                } else if ((x & 0x40) != 0) {
                    int index = integer(6);
                    String name = index == 0 ? string() : entry(index)[0];
                    String[] field = { name, string() };
                    out.add(field);
                    insert(field);
                    fields = true;
                } else if ((x & 0x20) != 0) {
                    if (fields) throw new Malformed("a size update of the dynamic table after a field");
                    int size = integer(5);
                    if (size > settingMax) throw new Malformed("a dynamic table of " + size + " bytes, over the " + settingMax + " allowed");
                    max = size;
                    evict();
                } else {
                    /* 0000xxxx: without indexing; 0001xxxx: never indexed. Neither changes the table. */
                    int index = integer(4);
                    String name = index == 0 ? string() : entry(index)[0];
                    out.add(new String[] { name, string() });
                    fields = true;
                }
            }
            b = null;
            return out;
        }

        private String[] entry(int index) throws IOException {
            if (index <= 0) throw new Malformed("index 0");
            if (index < STATIC.length) return STATIC[index];
            int d = index - STATIC.length;
            if (d >= dynamic.size()) throw new Malformed("index " + index + " past the dynamic table's " + dynamic.size() + " entries");
            return dynamic.get(d);
        }

        private void insert(String[] field) {
            int bytes = entrySize(field);
            if (bytes > max) {
                /* Larger than the table: the table is emptied, and nothing is added (RFC 7541, 4.4). */
                dynamic.clear();
                size = 0;
                return;
            }
            dynamic.add(0, field);
            size += bytes;
            evict();
        }

        private void evict() {
            while (size > max && !dynamic.isEmpty()) size -= entrySize(dynamic.remove(dynamic.size() - 1));
        }

        private static int entrySize(String[] field) {
            return field[0].getBytes(StandardCharsets.UTF_8).length + field[1].getBytes(StandardCharsets.UTF_8).length + 32;
        }

        private int integer(int prefix) throws IOException {
            if (at >= b.length) throw new Malformed("the block ends inside a number");
            int mask = (1 << prefix) - 1, value = b[at++] & mask;
            if (value < mask) return value;
            for (int shift = 0; ; shift += 7) {
                if (at >= b.length) throw new Malformed("the block ends inside a number");
                if (shift > 21) throw new Malformed("a number past 2^28");
                int x = b[at++] & 0xff;
                value += (x & 0x7f) << shift;
                if ((x & 0x80) == 0) return value;
            }
        }

        private String string() throws IOException {
            if (at >= b.length) throw new Malformed("the block ends before a string");
            boolean huffman = (b[at] & 0x80) != 0;
            int length = integer(7);
            if (length > b.length - at) throw new Malformed("a string of " + length + " bytes past the block's end");
            String s = huffman ? huffman(b, at, length) : new String(b, at, length, StandardCharsets.UTF_8);
            at += length;
            return s;
        }
    }

    /** Decodes a string in Huffman's code: its padding is the start of EOS, all ones, shorter than a byte. */
    static String huffman(byte[] b, int offset, int length) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(length * 8 / 5 + 1);
        int node = 0, depth = 0;
        boolean ones = true;
        for (int i = offset; i < offset + length; i++) {
            int x = b[i] & 0xff;
            for (int bit = 7; bit >= 0; bit--) {
                int v = (x >>> bit) & 1, next = TREE[2 * node + v];
                depth++;
                ones &= v == 1;
                if (next < 0) {
                    int symbol = -1 - next;
                    if (symbol == EOS) throw new Malformed("EOS inside a string");
                    out.write(symbol);
                    node = 0;
                    depth = 0;
                    ones = true;
                } else if (next == 0) {
                    throw new Malformed("no code of Huffman's");
                } else {
                    node = next;
                }
            }
        }
        if (depth > 7 || !ones) throw new Malformed("the padding of a string in Huffman's code");
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    /** Names whose values are never put in a table along the way (RFC 7541, 7.1.3). */
    private static boolean sensitive(String name) {
        return name.equals("authorization") || name.equals("proxy-authorization") || name.equals("cookie") || name.equals("set-cookie");
    }

    /** A header block of the fields given, each { name, value }, the names already in lower case. */
    static byte[] encode(List<String[]> fields) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(64 * fields.size() + 16);
        for (String[] f : fields) {
            out.write(sensitive(f[0]) ? 0x10 : 0x00);
            plain(out, f[0]);
            plain(out, f[1]);
        }
        return out.toByteArray();
    }

    private static void plain(ByteArrayOutputStream out, String s) {
        byte[] bytes = s.getBytes(StandardCharsets.UTF_8);
        integer(out, bytes.length, 7, 0);
        out.write(bytes, 0, bytes.length);
    }

    static void integer(ByteArrayOutputStream out, int value, int prefix, int first) {
        int mask = (1 << prefix) - 1;
        if (value < mask) {
            out.write(first | value);
            return;
        }
        out.write(first | mask);
        value -= mask;
        while (value >= 0x80) {
            out.write((value & 0x7f) | 0x80);
            value >>>= 7;
        }
        out.write(value);
    }
}
