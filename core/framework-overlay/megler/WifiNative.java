// Megler: new in the overlay. The phone's Wi-Fi over the shell's megler_wifi_host (core/include/megler.h), answered by
// core/src/drivers/mg_java_wifi.c; megler/Wifi.java is what calls it.
package megler;

public final class WifiNative {
    /* What state() answers: MEGLER_WIFI_* in megler.h. */
    public static final int NONE = 0, ON = 1, OFF = 2, DENIED = 3, UNKNOWN = 4;
    /* meta's indexes: the networks' number (-1: no scan), when Windows' report was made (ns since 1970). */
    public static final int M_COUNT = 0, M_TIME = 1, M_SIZE = 2;
    /* The numbers of each network in the int[] (the third of what scan() and connection() answer), I_STRIDE of them each. */
    public static final int I_SSID_SIZE = 0, I_RSSI = 1, I_FREQUENCY = 2, I_SECURITY = 3, I_PHY = 4, I_ADHOC = 5, I_STRIDE = 6;
    /* MEGLER_WIFI_AUTH_* (security & 0xff) and MEGLER_WIFI_CIPHER_* (security >> 8 & 0xff). */
    public static final int AUTH_UNKNOWN = 0, AUTH_OPEN = 1, AUTH_SHARED_KEY = 2, AUTH_WPA = 3, AUTH_WPA_PSK = 4, AUTH_RSNA = 5, AUTH_RSNA_PSK = 6,
            AUTH_WPA3 = 7, AUTH_WPA3_SAE = 8, AUTH_OWE = 9, AUTH_OTHER = 10;
    public static final int CIPHER_UNKNOWN = 0, CIPHER_NONE = 1, CIPHER_WEP = 2, CIPHER_TKIP = 3, CIPHER_CCMP = 4, CIPHER_GCMP = 5;
    /* MEGLER_WIFI_PHY_*, Windows' WiFiPhyKind. */
    public static final int PHY_UNKNOWN = 0, PHY_FHSS = 1, PHY_DSSS = 2, PHY_IR = 3, PHY_OFDM = 4, PHY_HRDSSS = 5, PHY_ERP = 6, PHY_HT = 7, PHY_VHT = 8,
            PHY_DMG = 9, PHY_HE = 10;

    private WifiNative() {}

    /** Whether the phone can scan; with `ask`, Windows is asked first if it has not been (this thread waits). */
    public static native int state(boolean ask);
    /**
     * A scan, waited for (a few seconds): {long[] BSSIDs, byte[] SSIDs (32 bytes each), int[] numbers (I_STRIDE each)}, its
     * number and time in meta (M_SIZE of them); null when no scan was made.
     */
    public static native Object[] scan(long[] meta);
    /** The network the phone is connected to by Wi-Fi, as scan() gives one; null when it is not on Wi-Fi. */
    public static native Object[] connection(long[] meta);
}
