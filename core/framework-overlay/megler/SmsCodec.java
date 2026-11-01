// Megler: GSM 7-bit text (3GPP TS 23.038), for the text messages megler.Phone makes and android.telephony.SmsMessage reads:
// the default alphabet and its extension table, packed into octets as a message's user data packs them.
package megler;

public final class SmsCodec {
    private SmsCodec() {}

    /** The default alphabet, by septet. */
    private static final String BASIC =
        "@£$¥èéùìòÇ\nØø\rÅå"
        + "Δ_ΦΓΛΩΠΨΣΘΞ\u001bÆæßÉ"
        + " !\"#¤%&'()*+,-./0123456789:;<=>?"
        + "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§"
        + "¿abcdefghijklmnopqrstuvwxyzäöñüà";
    /** The extension table: the septet after an escape (0x1B), and the character it stands for. */
    private static final String EXT_CODES = "\n\u0014()/<=>@e", EXT_CHARS = "\f^{}\\[~]|€";

    /** The septets of a text: each character of the default alphabet, the extension's after an escape, '?' for the rest. */
    public static int[] septets(String s) {
        int[] out = new int[s.length() * 2];
        int n = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            int at = BASIC.indexOf(c);
            if (at >= 0 && c != '\u001b') { out[n++] = at; continue; }
            int ext = EXT_CHARS.indexOf(c);
            if (ext >= 0) { out[n++] = 0x1B; out[n++] = EXT_CODES.charAt(ext); continue; }
            out[n++] = BASIC.indexOf('?');
        }
        return java.util.Arrays.copyOf(out, n);
    }

    /** A text's septets packed into octets, the first septet in the low bits of the first octet. */
    public static byte[] pack7(String s) {
        int[] sep = septets(s);
        byte[] out = new byte[(sep.length * 7 + 7) / 8];
        int bit = 0;
        for (int v : sep) {
            int at = bit >> 3, shift = bit & 7;
            out[at] |= (byte) (v << shift);
            if (shift > 1 && at + 1 < out.length) out[at + 1] |= (byte) (v >> (8 - shift));
            bit += 7;
        }
        return out;
    }

    /** `count` septets from `data` at `offset`, after `fill` bits (the padding a user data header leaves), as text. */
    public static String unpack7(byte[] data, int offset, int count, int fill) {
        StringBuilder b = new StringBuilder();
        boolean escaped = false;
        for (int i = 0; i < count; i++) {
            int bit = fill + i * 7, at = offset + (bit >> 3), shift = bit & 7;
            if (at >= data.length) break;
            int v = (data[at] & 0xFF) >> shift;
            if (shift > 1 && at + 1 < data.length) v |= (data[at + 1] & 0xFF) << (8 - shift);
            v &= 0x7F;
            if (escaped) {
                int ext = EXT_CODES.indexOf((char) v);
                b.append(ext >= 0 ? EXT_CHARS.charAt(ext) : ' ');
                escaped = false;
            } else if (v == 0x1B) {
                escaped = true;
            } else {
                b.append(BASIC.charAt(v));
            }
        }
        return b.toString();
    }
}
