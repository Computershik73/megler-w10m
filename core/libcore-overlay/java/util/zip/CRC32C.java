package java.util.zip;

import java.nio.ByteBuffer;

/**
 * CRC-32C (Castagnoli), as Android 14's java.util.zip has it (Java 9's). Android 9's libcore has none, and Yandex Maps
 * ended with NoClassDefFoundError when it saved the state of a route (package 489). A table, a byte at a time: what apps
 * check with it is small.
 */
public final class CRC32C implements Checksum {
    private static final int[] TABLE = new int[256];
    static {
        for (int n = 0; n < 256; n++) {
            int c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? (c >>> 1) ^ 0x82F63B78 : c >>> 1;
            TABLE[n] = c;
        }
    }

    private int crc = 0xFFFFFFFF;

    public CRC32C() {}

    @Override
    public void update(int b) {
        crc = (crc >>> 8) ^ TABLE[(crc ^ b) & 0xFF];
    }

    @Override
    public void update(byte[] b, int off, int len) {
        if (b == null) throw new NullPointerException();
        if (off < 0 || len < 0 || off > b.length - len) throw new ArrayIndexOutOfBoundsException();
        int c = crc;
        for (int i = off, end = off + len; i < end; i++) c = (c >>> 8) ^ TABLE[(c ^ b[i]) & 0xFF];
        crc = c;
    }

    @Override
    public void update(byte[] b) {
        update(b, 0, b.length);
    }

    @Override
    public void update(ByteBuffer buffer) {
        int pos = buffer.position(), limit = buffer.limit();
        if (pos > limit) return;
        if (buffer.hasArray()) {
            update(buffer.array(), buffer.arrayOffset() + pos, limit - pos);
        } else {
            int c = crc;
            for (int i = pos; i < limit; i++) c = (c >>> 8) ^ TABLE[(c ^ buffer.get(i)) & 0xFF];
            crc = c;
        }
        buffer.position(limit);
    }

    @Override
    public long getValue() {
        return ~crc & 0xFFFFFFFFL;
    }

    @Override
    public void reset() {
        crc = 0xFFFFFFFF;
    }
}
