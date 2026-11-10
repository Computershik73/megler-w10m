// Overlay of core/framework/gen-src/android/net/wifi/ScanResult.java (tools/build-framework.sh).
// It differs in being Android's: the generated stub kept its getters' values in a map nothing filled, answered every band
// question false and every conversion 0, copied nothing in its copy constructor, and wrote nothing into a Parcel (its
// CREATOR null). Here the fields are the scan's (megler.Wifi makes each result of the shell's scan): the BSSID and SSID,
// the network's WifiSsid, the capabilities, the signal, the channel's frequency and width, the time it was heard; and the
// band helpers, the channel conversions, the copy constructor, the Builder, toString and the Parcel are Android 14's.
// Every member of the stub is here.
//
// Why. Google Play services' network location finds where the phone is from a scan's results (WifiManager.getScanResults):
// their BSSIDs, signals and frequencies and the time each was heard -- it drops one heard before it asked for the scan, or
// over 30 seconds ago (geyw) -- and an access point whose SSID ends in "_nomap", as Google's rule of opting out has it.
package android.net.wifi;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class ScanResult implements android.os.Parcelable {
    public static final int BAND_24_GHZ_END_FREQ_MHZ = 2484;
    public static final int BAND_24_GHZ_FIRST_CH_NUM = 1;
    public static final int BAND_24_GHZ_LAST_CH_NUM = 14;
    public static final int BAND_24_GHZ_START_FREQ_MHZ = 2412;
    public static final int BAND_5_GHZ_END_FREQ_MHZ = 5885;
    public static final int BAND_5_GHZ_FIRST_CH_NUM = 32;
    public static final int BAND_5_GHZ_HIGH_LOWEST_FREQ_MHZ = 5500;
    public static final int BAND_5_GHZ_LAST_CH_NUM = 177;
    public static final int BAND_5_GHZ_LOW_HIGHEST_FREQ_MHZ = 5320;
    public static final int BAND_5_GHZ_START_FREQ_MHZ = 5160;
    public static final int BAND_60_GHZ_END_FREQ_MHZ = 70200;
    public static final int BAND_60_GHZ_FIRST_CH_NUM = 1;
    public static final int BAND_60_GHZ_LAST_CH_NUM = 6;
    public static final int BAND_60_GHZ_START_FREQ_MHZ = 58320;
    public static final int BAND_6_GHZ_END_FREQ_MHZ = 7115;
    public static final int BAND_6_GHZ_FIRST_CH_NUM = 1;
    public static final int BAND_6_GHZ_LAST_CH_NUM = 233;
    public static final int BAND_6_GHZ_OP_CLASS_136_CH_2_FREQ_MHZ = 5935;
    public static final int BAND_6_GHZ_PSC_START_MHZ = 5975;
    public static final int BAND_6_GHZ_PSC_STEP_SIZE_MHZ = 80;
    public static final int BAND_6_GHZ_START_FREQ_MHZ = 5955;
    public static final int CHANNEL_WIDTH_160MHZ = 3;
    public static final int CHANNEL_WIDTH_20MHZ = 0;
    public static final int CHANNEL_WIDTH_320MHZ = 5;
    public static final int CHANNEL_WIDTH_40MHZ = 1;
    public static final int CHANNEL_WIDTH_80MHZ = 2;
    public static final int CHANNEL_WIDTH_80MHZ_PLUS_MHZ = 4;
    public static final int CIPHER_BIP_CMAC_256 = 9;
    public static final int CIPHER_BIP_GMAC_128 = 7;
    public static final int CIPHER_BIP_GMAC_256 = 8;
    public static final int CIPHER_CCMP = 3;
    public static final int CIPHER_CCMP_256 = 10;
    public static final int CIPHER_GCMP_128 = 6;
    public static final int CIPHER_GCMP_256 = 4;
    public static final int CIPHER_NONE = 0;
    public static final int CIPHER_NO_GROUP_ADDRESSED = 1;
    public static final int CIPHER_SMS4 = 5;
    public static final int CIPHER_TKIP = 2;
    public static final long FLAG_80211az_NTB_RESPONDER = 4L;
    public static final long FLAG_80211mc_RESPONDER = 2L;
    public static final long FLAG_PASSPOINT_NETWORK = 1L;
    public static final long FLAG_RANGING_FRAME_PROTECTION_REQUIRED = 32L;
    public static final long FLAG_SECURE_HE_LTF_SUPPORTED = 16L;
    public static final long FLAG_TWT_RESPONDER = 8L;
    public static final int KEY_MGMT_DPP = 17;
    public static final int KEY_MGMT_EAP = 2;
    public static final int KEY_MGMT_EAP_FT_SHA384 = 21;
    public static final int KEY_MGMT_EAP_SHA256 = 6;
    public static final int KEY_MGMT_EAP_SUITE_B_192 = 10;
    public static final int KEY_MGMT_FILS_SHA256 = 15;
    public static final int KEY_MGMT_FILS_SHA384 = 16;
    public static final int KEY_MGMT_FT_EAP = 4;
    public static final int KEY_MGMT_FT_PSK = 3;
    public static final int KEY_MGMT_FT_PSK_SHA384 = 22;
    public static final int KEY_MGMT_FT_SAE = 11;
    public static final int KEY_MGMT_FT_SAE_EXT_KEY = 19;
    public static final int KEY_MGMT_NONE = 0;
    public static final int KEY_MGMT_OSEN = 7;
    public static final int KEY_MGMT_OWE = 9;
    public static final int KEY_MGMT_OWE_TRANSITION = 12;
    public static final int KEY_MGMT_PASN = 20;
    public static final int KEY_MGMT_PSK = 1;
    public static final int KEY_MGMT_PSK_SHA256 = 5;
    public static final int KEY_MGMT_SAE = 8;
    public static final int KEY_MGMT_SAE_EXT_KEY = 18;
    public static final int KEY_MGMT_UNKNOWN = 23;
    public static final int KEY_MGMT_WAPI_CERT = 14;
    public static final int KEY_MGMT_WAPI_PSK = 13;
    public static final int PREAMBLE_EHT = 4;
    public static final int PREAMBLE_HE = 3;
    public static final int PREAMBLE_HT = 1;
    public static final int PREAMBLE_LEGACY = 0;
    public static final int PREAMBLE_VHT = 2;
    public static final int PROTOCOL_NONE = 0;
    public static final int PROTOCOL_OSEN = 3;
    public static final int PROTOCOL_RSN = 2;
    public static final int PROTOCOL_WAPI = 4;
    public static final int PROTOCOL_WPA = 1;
    public static final int UNSPECIFIED = -1;
    public static final int WIFI_BAND_24_GHZ = 1;
    public static final int WIFI_BAND_5_GHZ = 2;
    public static final int WIFI_BAND_5_GHZ_HIGH = 1073741824;
    public static final int WIFI_BAND_5_GHZ_LOW = 536870912;
    public static final int WIFI_BAND_60_GHZ = 16;
    public static final int WIFI_BAND_6_GHZ = 8;
    public static final int WIFI_STANDARD_11AC = 5;
    public static final int WIFI_STANDARD_11AD = 7;
    public static final int WIFI_STANDARD_11AX = 6;
    public static final int WIFI_STANDARD_11BE = 8;
    public static final int WIFI_STANDARD_11N = 4;
    public static final int WIFI_STANDARD_LEGACY = 1;
    public static int WIFI_STANDARD_UNKNOWN = 0;
    public java.lang.String BSSID;
    public java.lang.String SSID;
    public int anqpDomainId;
    public android.net.wifi.AnqpInformationElement[] anqpElements;
    public java.util.List anqpLines;
    public java.lang.String capabilities;
    public int centerFreq0;
    public int centerFreq1;
    public int channelWidth;
    public int distanceCm;
    public int distanceSdCm;
    public long flags;
    public int frequency;
    public long hessid;
    public java.lang.String ifaceName;
    public android.net.wifi.ScanResult.InformationElement[] informationElements;
    public boolean is80211McRTTResponder;
    public int level;
    public int numUsage;
    public java.lang.CharSequence operatorFriendlyName;
    public android.net.wifi.ScanResult.RadioChainInfo[] radioChainInfos;
    /** When it was last heard, milliseconds since 1970 (System.currentTimeMillis's clock). */
    public long seen;
    /** When it was last heard, microseconds since boot (SystemClock.elapsedRealtime's clock). */
    public long timestamp;
    public boolean untrusted;
    public java.lang.CharSequence venueName;
    public android.net.wifi.WifiSsid wifiSsid;
    private int mWifiStandard = WIFI_STANDARD_UNKNOWN;
    private java.util.List mAffiliatedMloLinks = new java.util.ArrayList();
    private android.net.MacAddress mApMldMacAddress;
    private int mApMloLinkId = -1;

    public ScanResult() {}

    public ScanResult(android.net.wifi.ScanResult source) {
        if (source == null) return;
        wifiSsid = source.wifiSsid;
        SSID = source.SSID;
        BSSID = source.BSSID;
        hessid = source.hessid;
        anqpDomainId = source.anqpDomainId;
        capabilities = source.capabilities;
        level = source.level;
        frequency = source.frequency;
        channelWidth = source.channelWidth;
        centerFreq0 = source.centerFreq0;
        centerFreq1 = source.centerFreq1;
        timestamp = source.timestamp;
        distanceCm = source.distanceCm;
        distanceSdCm = source.distanceSdCm;
        seen = source.seen;
        untrusted = source.untrusted;
        numUsage = source.numUsage;
        venueName = source.venueName;
        operatorFriendlyName = source.operatorFriendlyName;
        flags = source.flags;
        is80211McRTTResponder = source.is80211McRTTResponder;
        informationElements = source.informationElements == null ? null : source.informationElements.clone();
        anqpLines = source.anqpLines;
        anqpElements = source.anqpElements;
        radioChainInfos = source.radioChainInfos == null ? null : source.radioChainInfos.clone();
        ifaceName = source.ifaceName;
        mWifiStandard = source.mWifiStandard;
        mAffiliatedMloLinks = new java.util.ArrayList(source.mAffiliatedMloLinks);
        mApMldMacAddress = source.mApMldMacAddress;
        mApMloLinkId = source.mApMloLinkId;
    }

    public ScanResult(android.net.wifi.WifiSsid wifiSsid, java.lang.String BSSID, long hessid, int anqpDomainId, byte[] osuProviders, java.lang.String caps,
                      int level, int frequency, long tsf) {
        setWifiSsid(wifiSsid);
        this.BSSID = BSSID;
        this.hessid = hessid;
        this.anqpDomainId = anqpDomainId;
        this.capabilities = caps;
        this.level = level;
        this.frequency = frequency;
        this.timestamp = tsf;
        this.channelWidth = UNSPECIFIED;
        this.centerFreq0 = UNSPECIFIED;
        this.centerFreq1 = UNSPECIFIED;
    }

    public ScanResult(android.net.wifi.WifiSsid wifiSsid, java.lang.String BSSID, java.lang.String caps, int level, int frequency, long tsf, int distCm,
                      int distSdCm) {
        setWifiSsid(wifiSsid);
        this.BSSID = BSSID;
        this.capabilities = caps;
        this.level = level;
        this.frequency = frequency;
        this.timestamp = tsf;
        this.distanceCm = distCm;
        this.distanceSdCm = distSdCm;
        this.channelWidth = UNSPECIFIED;
        this.centerFreq0 = UNSPECIFIED;
        this.centerFreq1 = UNSPECIFIED;
    }

    public ScanResult(android.net.wifi.WifiSsid wifiSsid, java.lang.String SSID, java.lang.String BSSID, long hessid, int anqpDomainId,
                      java.lang.String caps, int level, int frequency, long tsf, int distCm, int distSdCm, int channelWidth, int centerFreq0,
                      int centerFreq1, boolean is80211McResponder) {
        this(SSID, BSSID, hessid, anqpDomainId, caps, level, frequency, tsf, distCm, distSdCm, channelWidth, centerFreq0, centerFreq1, is80211McResponder);
        this.wifiSsid = wifiSsid;
    }

    public ScanResult(java.lang.String SSID, java.lang.String BSSID, long hessid, int anqpDomainId, java.lang.String caps, int level, int frequency,
                      long tsf, int distCm, int distSdCm, int channelWidth, int centerFreq0, int centerFreq1, boolean is80211McResponder) {
        this.SSID = SSID;
        this.BSSID = BSSID;
        this.hessid = hessid;
        this.anqpDomainId = anqpDomainId;
        this.capabilities = caps;
        this.level = level;
        this.frequency = frequency;
        this.timestamp = tsf;
        this.distanceCm = distCm;
        this.distanceSdCm = distSdCm;
        this.channelWidth = channelWidth;
        this.centerFreq0 = centerFreq0;
        this.centerFreq1 = centerFreq1;
        if (is80211McResponder) setFlag(FLAG_80211mc_RESPONDER);
    }

    /* Android's bands and channels (ScanResult's own helpers). */
    public static boolean is24GHz(int freqMhz) { return freqMhz >= BAND_24_GHZ_START_FREQ_MHZ && freqMhz <= BAND_24_GHZ_END_FREQ_MHZ; }
    public static boolean is5GHz(int freqMhz) { return freqMhz >= BAND_5_GHZ_START_FREQ_MHZ && freqMhz <= BAND_5_GHZ_END_FREQ_MHZ; }
    public static boolean is6GHz(int freqMhz) {
        return freqMhz == BAND_6_GHZ_OP_CLASS_136_CH_2_FREQ_MHZ || (freqMhz >= BAND_6_GHZ_START_FREQ_MHZ && freqMhz <= BAND_6_GHZ_END_FREQ_MHZ);
    }
    public static boolean is60GHz(int freqMhz) { return freqMhz >= BAND_60_GHZ_START_FREQ_MHZ && freqMhz <= BAND_60_GHZ_END_FREQ_MHZ; }
    public static boolean is6GHzPsc(int freqMhz) {
        return is6GHz(freqMhz) && freqMhz != BAND_6_GHZ_OP_CLASS_136_CH_2_FREQ_MHZ && (freqMhz - BAND_6_GHZ_PSC_START_MHZ) % BAND_6_GHZ_PSC_STEP_SIZE_MHZ == 0;
    }
    public static boolean isValidCombinedBandForDual5GHz(int band1, int band2) {
        return (band1 == WIFI_BAND_5_GHZ_LOW && band2 == WIFI_BAND_5_GHZ_HIGH) || (band1 == WIFI_BAND_5_GHZ_HIGH && band2 == WIFI_BAND_5_GHZ_LOW);
    }
    public static int toBand(int freqMhz) {
        if (is24GHz(freqMhz)) return WIFI_BAND_24_GHZ;
        if (is5GHz(freqMhz)) return WIFI_BAND_5_GHZ;
        if (is6GHz(freqMhz)) return WIFI_BAND_6_GHZ;
        if (is60GHz(freqMhz)) return WIFI_BAND_60_GHZ;
        return UNSPECIFIED;
    }
    public static int getBandFromOpClass(int opClass, int channel) {
        if (opClass >= 81 && opClass <= 84) return WIFI_BAND_24_GHZ;
        if (opClass >= 115 && opClass <= 130) return WIFI_BAND_5_GHZ;
        if (opClass >= 131 && opClass <= 137) return WIFI_BAND_6_GHZ;
        if (opClass >= 180 && opClass <= 181) return WIFI_BAND_60_GHZ;
        return UNSPECIFIED;
    }
    public static int convertFrequencyMhzToChannelIfSupported(int freqMhz) {
        if (freqMhz == 2484) return 14;
        if (is24GHz(freqMhz)) return (freqMhz - BAND_24_GHZ_START_FREQ_MHZ) / 5 + BAND_24_GHZ_FIRST_CH_NUM;
        if (is5GHz(freqMhz)) return (freqMhz - BAND_5_GHZ_START_FREQ_MHZ) / 5 + BAND_5_GHZ_FIRST_CH_NUM;
        if (is6GHz(freqMhz)) return freqMhz == BAND_6_GHZ_OP_CLASS_136_CH_2_FREQ_MHZ ? 2 : (freqMhz - BAND_6_GHZ_START_FREQ_MHZ) / 5 + BAND_6_GHZ_FIRST_CH_NUM;
        if (is60GHz(freqMhz)) return (freqMhz - BAND_60_GHZ_START_FREQ_MHZ) / 2160 + BAND_60_GHZ_FIRST_CH_NUM;
        return UNSPECIFIED;
    }
    public static int convertChannelToFrequencyMhzIfSupported(int channel, int band) {
        if (band == WIFI_BAND_24_GHZ) {
            if (channel == 14) return 2484;
            if (channel >= BAND_24_GHZ_FIRST_CH_NUM && channel <= 13) return (channel - BAND_24_GHZ_FIRST_CH_NUM) * 5 + BAND_24_GHZ_START_FREQ_MHZ;
            return UNSPECIFIED;
        }
        if (band == WIFI_BAND_5_GHZ) {
            if (channel >= BAND_5_GHZ_FIRST_CH_NUM && channel <= BAND_5_GHZ_LAST_CH_NUM) return (channel - BAND_5_GHZ_FIRST_CH_NUM) * 5 + BAND_5_GHZ_START_FREQ_MHZ;
            return UNSPECIFIED;
        }
        if (band == WIFI_BAND_6_GHZ) {
            if (channel == 2) return BAND_6_GHZ_OP_CLASS_136_CH_2_FREQ_MHZ;
            if (channel >= BAND_6_GHZ_FIRST_CH_NUM && channel <= BAND_6_GHZ_LAST_CH_NUM) return (channel - BAND_6_GHZ_FIRST_CH_NUM) * 5 + BAND_6_GHZ_START_FREQ_MHZ;
            return UNSPECIFIED;
        }
        if (band == WIFI_BAND_60_GHZ) {
            if (channel >= BAND_60_GHZ_FIRST_CH_NUM && channel <= BAND_60_GHZ_LAST_CH_NUM) return (channel - BAND_60_GHZ_FIRST_CH_NUM) * 2160 + BAND_60_GHZ_START_FREQ_MHZ;
            return UNSPECIFIED;
        }
        return UNSPECIFIED;
    }
    public static java.lang.String wifiStandardToString(int standard) {
        switch (standard) {
        case WIFI_STANDARD_LEGACY: return "legacy";
        case WIFI_STANDARD_11N: return "11n";
        case WIFI_STANDARD_11AC: return "11ac";
        case WIFI_STANDARD_11AX: return "11ax";
        case WIFI_STANDARD_11AD: return "11ad";
        case WIFI_STANDARD_11BE: return "11be";
        default: return "unknown";
        }
    }

    public void clearFlag(long flag) { flags &= ~flag; }
    public void setFlag(long flag) { flags |= flag; }
    public int describeContents() { return 0; }
    public java.util.List getAffiliatedMloLinks() { return new java.util.ArrayList(mAffiliatedMloLinks); }
    public android.net.MacAddress getApMldMacAddress() { return mApMldMacAddress; }
    public int getApMloLinkId() { return mApMloLinkId; }
    public int getBand() { return toBand(frequency); }
    public java.util.List getInformationElements() {
        return informationElements == null ? new java.util.ArrayList() : new java.util.ArrayList(java.util.Arrays.asList(informationElements));
    }
    /** WifiInfo's security types (WifiInfo.SECURITY_TYPE_*) of the capabilities: open 0, WEP 1, PSK 2, EAP 3, SAE 4, OWE 6. */
    public int[] getSecurityTypes() {
        String c = capabilities == null ? "" : capabilities;
        java.util.ArrayList<Integer> t = new java.util.ArrayList<>();
        if (c.contains("WEP")) t.add(1);
        if (c.contains("-PSK")) t.add(2);
        if (c.contains("-EAP")) t.add(3);
        if (c.contains("-SAE")) t.add(4);
        if (c.contains("-OWE")) t.add(6);
        if (t.isEmpty()) t.add(0);
        int[] out = new int[t.size()];
        for (int i = 0; i < out.length; i++) out[i] = t.get(i);
        return out;
    }
    public android.net.wifi.WifiSsid getWifiSsid() { return wifiSsid; }
    public int getWifiStandard() { return mWifiStandard; }
    public boolean is24GHz() { return is24GHz(frequency); }
    public boolean is5GHz() { return is5GHz(frequency); }
    public boolean is60GHz() { return is60GHz(frequency); }
    public boolean is6GHz() { return is6GHz(frequency); }
    public boolean is6GhzPsc() { return is6GHzPsc(frequency); }
    public boolean is80211azNtbResponder() { return (flags & FLAG_80211az_NTB_RESPONDER) != 0; }
    public boolean is80211mcResponder() { return (flags & FLAG_80211mc_RESPONDER) != 0; }
    public boolean isPasspointNetwork() { return (flags & FLAG_PASSPOINT_NETWORK) != 0; }
    public boolean isRangingFrameProtectionRequired() { return (flags & FLAG_RANGING_FRAME_PROTECTION_REQUIRED) != 0; }
    public boolean isSecureHeLtfSupported() { return (flags & FLAG_SECURE_HE_LTF_SUPPORTED) != 0; }
    public boolean isTwtResponder() { return (flags & FLAG_TWT_RESPONDER) != 0; }
    public void setAffiliatedMloLinks(java.util.List links) { mAffiliatedMloLinks = new java.util.ArrayList(links == null ? new java.util.ArrayList() : links); }
    public void setApMldMacAddress(android.net.MacAddress a) { mApMldMacAddress = a; }
    public void setApMloLinkId(int id) { mApMloLinkId = id; }
    /** The network's name: its WifiSsid, and SSID its text as Android has it (the UTF-8 text, empty for a hidden network). */
    public void setWifiSsid(android.net.wifi.WifiSsid ssid) {
        wifiSsid = ssid;
        SSID = ssid == null ? WifiManager.UNKNOWN_SSID : ssid.meglerText();
    }
    public void setWifiStandard(int standard) { mWifiStandard = standard; }

    @Override
    public String toString() {
        return "SSID: " + (wifiSsid == null ? WifiManager.UNKNOWN_SSID : wifiSsid) + ", BSSID: " + (BSSID == null ? "<none>" : BSSID) + ", capabilities: "
            + (capabilities == null ? "<none>" : capabilities) + ", level: " + level + ", frequency: " + frequency + ", timestamp: " + timestamp
            + ", distance: " + (distanceCm != UNSPECIFIED ? distanceCm : "?") + "(cm), distanceSd: " + (distanceSdCm != UNSPECIFIED ? distanceSdCm : "?")
            + "(cm), passpoint: " + (isPasspointNetwork() ? "yes" : "no") + ", ChannelBandwidth: " + channelWidth + ", centerFreq0: " + centerFreq0
            + ", centerFreq1: " + centerFreq1 + ", standard: " + wifiStandardToString(mWifiStandard) + ", 80211mcResponder: "
            + (is80211mcResponder() ? "is supported" : "is not supported");
    }

    /* Android's layout, of what a scan here fills in: the name's octets, then the rest field by field. */
    public void writeToParcel(android.os.Parcel dest, int flags) {
        dest.writeByteArray(wifiSsid == null ? null : wifiSsid.getBytes());
        dest.writeString(SSID);
        dest.writeString(BSSID);
        dest.writeLong(hessid);
        dest.writeInt(anqpDomainId);
        dest.writeString(capabilities);
        dest.writeInt(level);
        dest.writeInt(frequency);
        dest.writeLong(timestamp);
        dest.writeInt(distanceCm);
        dest.writeInt(distanceSdCm);
        dest.writeInt(channelWidth);
        dest.writeInt(centerFreq0);
        dest.writeInt(centerFreq1);
        dest.writeInt(mWifiStandard);
        dest.writeLong(seen);
        dest.writeInt(untrusted ? 1 : 0);
        dest.writeInt(numUsage);
        dest.writeString(venueName == null ? null : venueName.toString());
        dest.writeString(operatorFriendlyName == null ? null : operatorFriendlyName.toString());
        dest.writeLong(this.flags);
        dest.writeString(ifaceName);
        dest.writeInt(mApMloLinkId);
    }

    public static final android.os.Parcelable.Creator<ScanResult> CREATOR = new android.os.Parcelable.Creator<ScanResult>() {
        public ScanResult createFromParcel(android.os.Parcel in) {
            ScanResult r = new ScanResult();
            byte[] octets = in.createByteArray();
            if (octets != null) r.wifiSsid = WifiSsid.fromBytes(octets);
            r.SSID = in.readString();
            r.BSSID = in.readString();
            r.hessid = in.readLong();
            r.anqpDomainId = in.readInt();
            r.capabilities = in.readString();
            r.level = in.readInt();
            r.frequency = in.readInt();
            r.timestamp = in.readLong();
            r.distanceCm = in.readInt();
            r.distanceSdCm = in.readInt();
            r.channelWidth = in.readInt();
            r.centerFreq0 = in.readInt();
            r.centerFreq1 = in.readInt();
            r.mWifiStandard = in.readInt();
            r.seen = in.readLong();
            r.untrusted = in.readInt() != 0;
            r.numUsage = in.readInt();
            r.venueName = in.readString();
            r.operatorFriendlyName = in.readString();
            r.flags = in.readLong();
            r.ifaceName = in.readString();
            r.mApMloLinkId = in.readInt();
            return r;
        }
        public ScanResult[] newArray(int size) { return new ScanResult[size]; }
    };

    public static final class Builder {
        private WifiSsid mWifiSsid;
        private String mBssid, mCaps;
        private long mHessid, mTsf;
        private int mAnqpDomainId, mRssi, mFrequency, mDistanceCm = UNSPECIFIED, mDistanceSdCm = UNSPECIFIED, mChannelWidth = UNSPECIFIED,
                mCenterFreq0 = UNSPECIFIED, mCenterFreq1 = UNSPECIFIED;
        private boolean mIs80211McRTTResponder, mIs80211azNtbRTTResponder, mIsTwtResponder, mRangingFrameProtectionRequired, mSecureHeLtfSupported;
        public Builder() {}
        public Builder(android.net.wifi.WifiSsid wifiSsid, java.lang.String bssid) {
            mWifiSsid = wifiSsid;
            mBssid = bssid;
        }
        public android.net.wifi.ScanResult build() {
            ScanResult r = new ScanResult(mWifiSsid, mBssid, mCaps, mRssi, mFrequency, mTsf, mDistanceCm, mDistanceSdCm);
            r.hessid = mHessid;
            r.anqpDomainId = mAnqpDomainId;
            r.channelWidth = mChannelWidth;
            r.centerFreq0 = mCenterFreq0;
            r.centerFreq1 = mCenterFreq1;
            if (mIs80211McRTTResponder) r.setFlag(FLAG_80211mc_RESPONDER);
            if (mIs80211azNtbRTTResponder) r.setFlag(FLAG_80211az_NTB_RESPONDER);
            if (mIsTwtResponder) r.setFlag(FLAG_TWT_RESPONDER);
            if (mRangingFrameProtectionRequired) r.setFlag(FLAG_RANGING_FRAME_PROTECTION_REQUIRED);
            if (mSecureHeLtfSupported) r.setFlag(FLAG_SECURE_HE_LTF_SUPPORTED);
            return r;
        }
        public void clear() {
            mWifiSsid = null;
            mBssid = mCaps = null;
            mHessid = mTsf = 0;
            mAnqpDomainId = mRssi = mFrequency = 0;
            mDistanceCm = mDistanceSdCm = mChannelWidth = mCenterFreq0 = mCenterFreq1 = UNSPECIFIED;
            mIs80211McRTTResponder = mIs80211azNtbRTTResponder = mIsTwtResponder = mRangingFrameProtectionRequired = mSecureHeLtfSupported = false;
        }
        public android.net.wifi.ScanResult.Builder setAnqpDomainId(int p0) { mAnqpDomainId = p0; return this; }
        public android.net.wifi.ScanResult.Builder setBssid(java.lang.String p0) { mBssid = p0; return this; }
        public android.net.wifi.ScanResult.Builder setCaps(java.lang.String p0) { mCaps = p0; return this; }
        public android.net.wifi.ScanResult.Builder setCenterFreq0(int p0) { mCenterFreq0 = p0; return this; }
        public android.net.wifi.ScanResult.Builder setCenterFreq1(int p0) { mCenterFreq1 = p0; return this; }
        public android.net.wifi.ScanResult.Builder setChannelWidth(int p0) { mChannelWidth = p0; return this; }
        public android.net.wifi.ScanResult.Builder setDistanceCm(int p0) { mDistanceCm = p0; return this; }
        public android.net.wifi.ScanResult.Builder setDistanceSdCm(int p0) { mDistanceSdCm = p0; return this; }
        public android.net.wifi.ScanResult.Builder setFrequency(int p0) { mFrequency = p0; return this; }
        public android.net.wifi.ScanResult.Builder setHessid(long p0) { mHessid = p0; return this; }
        public android.net.wifi.ScanResult.Builder setIs80211McRTTResponder(boolean p0) { mIs80211McRTTResponder = p0; return this; }
        public android.net.wifi.ScanResult.Builder setIs80211azNtbRTTResponder(boolean p0) { mIs80211azNtbRTTResponder = p0; return this; }
        public android.net.wifi.ScanResult.Builder setIsTwtResponder(boolean p0) { mIsTwtResponder = p0; return this; }
        public android.net.wifi.ScanResult.Builder setOsuProviders(byte[] p0) { return this; }
        public android.net.wifi.ScanResult.Builder setRangingFrameProtectionRequired(boolean p0) { mRangingFrameProtectionRequired = p0; return this; }
        public android.net.wifi.ScanResult.Builder setRssi(int p0) { mRssi = p0; return this; }
        public android.net.wifi.ScanResult.Builder setSecureHeLtfSupported(boolean p0) { mSecureHeLtfSupported = p0; return this; }
        public android.net.wifi.ScanResult.Builder setTsf(long p0) { mTsf = p0; return this; }
        public android.net.wifi.ScanResult.Builder setWifiSsid(android.net.wifi.WifiSsid p0) { mWifiSsid = p0; return this; }
    }

    public static class InformationElement implements android.os.Parcelable {
        public static final int EID_BSS_LOAD = 11;
        public static final int EID_COUNTRY = 7;
        public static final int EID_ERP = 42;
        public static final int EID_EXTENDED_CAPS = 127;
        public static final int EID_EXTENDED_SUPPORTED_RATES = 50;
        public static final int EID_EXTENSION_PRESENT = 255;
        public static final int EID_EXT_EHT_CAPABILITIES = 108;
        public static final int EID_EXT_EHT_OPERATION = 106;
        public static final int EID_EXT_HE_CAPABILITIES = 35;
        public static final int EID_EXT_HE_OPERATION = 36;
        public static final int EID_EXT_MULTI_LINK = 107;
        public static final int EID_FRAGMENT_SUB_ELEMENT_MULTI_LINK = 254;
        public static final int EID_HT_CAPABILITIES = 45;
        public static final int EID_HT_OPERATION = 61;
        public static final int EID_INTERWORKING = 107;
        public static final int EID_RNR = 201;
        public static final int EID_ROAMING_CONSORTIUM = 111;
        public static final int EID_RSN = 48;
        public static final int EID_RSN_EXTENSION = 244;
        public static final int EID_SSID = 0;
        public static final int EID_SUPPORTED_RATES = 1;
        public static final int EID_TIM = 5;
        public static final int EID_VHT_CAPABILITIES = 191;
        public static final int EID_VHT_OPERATION = 192;
        public static final int EID_VSA = 221;
        public byte[] bytes;
        public int id;
        public int idExt;
        public InformationElement() {}
        public InformationElement(int id, int idExt, byte[] bytes) {
            this.id = id;
            this.idExt = idExt;
            this.bytes = bytes == null ? null : bytes.clone();
        }
        public InformationElement(android.net.wifi.ScanResult.InformationElement e) {
            this(e.id, e.idExt, e.bytes);
        }
        public int describeContents() { return 0; }
        public java.nio.ByteBuffer getBytes() { return java.nio.ByteBuffer.wrap(bytes == null ? new byte[0] : bytes).asReadOnlyBuffer(); }
        public int getId() { return id; }
        public int getIdExt() { return idExt; }
        public void writeToParcel(android.os.Parcel dest, int flags) {
            dest.writeInt(id);
            dest.writeInt(idExt);
            dest.writeByteArray(bytes);
        }
        public static final android.os.Parcelable.Creator<InformationElement> CREATOR = new android.os.Parcelable.Creator<InformationElement>() {
            public InformationElement createFromParcel(android.os.Parcel in) {
                InformationElement e = new InformationElement();
                e.id = in.readInt();
                e.idExt = in.readInt();
                e.bytes = in.createByteArray();
                return e;
            }
            public InformationElement[] newArray(int size) { return new InformationElement[size]; }
        };
    }

    public static class RadioChainInfo {
        public int id;
        public int level;
        public RadioChainInfo() {}
    }
}
