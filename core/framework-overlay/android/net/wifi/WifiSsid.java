// Overlay of core/framework/gen-src/android/net/wifi/WifiSsid.java (tools/build-framework.sh).
// It differs in holding the name: the generated stub made an empty WifiSsid of anything, answered null for its bytes and its
// text, and wrote nothing into a Parcel. Here a WifiSsid is a network's name as its octets (up to 32), as Android 13's is:
// fromBytes, fromString (quoted UTF-8, or hex), fromUtf8Text, the older createFromAsciiEncoded, getBytes, getUtf8Text,
// toString quoted where the octets are UTF-8 and hex where not, equals and hashCode by the octets, and the Parcel.
// meglerText is the name as ScanResult.SSID has it: the UTF-8 text unquoted, malformed bytes replaced, "" for a hidden
// network (Android 9's WifiSsid.toString, which ScanResult.SSID still is).
//
// Why. A scan's results carry their names as WifiSsids (ScanResult.getWifiSsid), and WifiManager.getConnectionInfo's network
// its (WifiInfo.getWifiSsid); megler.Wifi makes both of a scan's octets.
package android.net.wifi;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class WifiSsid implements android.os.Parcelable {
    public static final java.lang.String NONE = "<unknown ssid>";
    /** The octets (Android 12's hidden field, kept for code that reads it). */
    public java.io.ByteArrayOutputStream octets = new java.io.ByteArrayOutputStream(32);
    private final byte[] mBytes;

    protected WifiSsid() { this(new byte[0]); }

    private WifiSsid(byte[] bytes) {
        mBytes = bytes;
        octets.write(bytes, 0, bytes.length);
    }

    public static android.net.wifi.WifiSsid fromBytes(byte[] bytes) {
        if (bytes == null) bytes = new byte[0];
        if (bytes.length > 32) throw new IllegalArgumentException("A WifiSsid must be 32 bytes or fewer: " + bytes.length);
        return new WifiSsid(bytes.clone());
    }

    /** Of "\"text\"" (UTF-8) or of hex digits, as toString writes it. */
    public static android.net.wifi.WifiSsid fromString(java.lang.String s) {
        if (s == null) throw new IllegalArgumentException("null SSID");
        if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) return fromUtf8Text(s.substring(1, s.length() - 1));
        if (s.length() % 2 != 0) throw new IllegalArgumentException("not hex: " + s);
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            int hi = Character.digit(s.charAt(2 * i), 16), lo = Character.digit(s.charAt(2 * i + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("not hex: " + s);
            b[i] = (byte) (hi << 4 | lo);
        }
        return fromBytes(b);
    }

    /** Of text, as UTF-8, cut at 32 bytes. */
    public static android.net.wifi.WifiSsid fromUtf8Text(java.lang.CharSequence text) {
        byte[] b = text == null ? new byte[0] : text.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
        return new WifiSsid(java.util.Arrays.copyOf(b, Math.min(b.length, 32)));
    }

    /** Android 9's: of text, its characters as UTF-8. */
    public static android.net.wifi.WifiSsid createFromAsciiEncoded(java.lang.String text) { return fromUtf8Text(text); }

    public byte[] getBytes() { return mBytes.clone(); }

    public byte[] getOctets() { return getBytes(); }

    /** The octets as UTF-8 text, or null when they are not UTF-8. */
    public java.lang.CharSequence getUtf8Text() {
        java.nio.charset.CharsetDecoder d = java.nio.charset.StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT).onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT);
        try {
            return d.decode(java.nio.ByteBuffer.wrap(mBytes)).toString();
        } catch (java.nio.charset.CharacterCodingException e) {
            return null;
        }
    }

    /** The name as ScanResult.SSID has it: the text, malformed octets replaced; "" for none, or for a name of zeros. */
    public java.lang.String meglerText() {
        boolean zeros = true;
        for (byte b : mBytes) zeros &= b == 0;
        if (mBytes.length == 0 || zeros) return "";
        return new String(mBytes, java.nio.charset.StandardCharsets.UTF_8);
    }

    @Override
    public String toString() {
        CharSequence t = getUtf8Text();
        if (t != null) return "\"" + t + "\"";
        StringBuilder b = new StringBuilder(mBytes.length * 2);
        for (byte x : mBytes) b.append(Character.forDigit((x >> 4) & 15, 16)).append(Character.forDigit(x & 15, 16));
        return b.toString();
    }

    @Override
    public boolean equals(Object o) { return o instanceof WifiSsid && java.util.Arrays.equals(mBytes, ((WifiSsid) o).mBytes); }

    @Override
    public int hashCode() { return java.util.Arrays.hashCode(mBytes); }

    public int describeContents() { return 0; }

    public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeByteArray(mBytes); }

    public static final android.os.Parcelable.Creator<WifiSsid> CREATOR = new android.os.Parcelable.Creator<WifiSsid>() {
        public WifiSsid createFromParcel(android.os.Parcel in) {
            byte[] b = in.createByteArray();
            return new WifiSsid(b == null ? new byte[0] : b);
        }
        public WifiSsid[] newArray(int size) { return new WifiSsid[size]; }
    };
}
