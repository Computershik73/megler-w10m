// Overlay of core/framework/gen-src/android/telephony/SmsMessage.java, the generated stub (tools/build-framework.sh).
// It differs in reading a message: createFromPdu made an empty SmsMessage, whose sender, text and time were null and 0.
// Now a 3GPP PDU (TS 23.040: SMS-DELIVER, as a phone receives one) is read -- the service centre, the sender (a number, or
// letters in GSM 7-bit), the protocol identifier, the coding (GSM 7-bit, 8-bit data, UCS-2), the time, a user data header
// (skipped, with the padding it leaves in 7-bit text) and the text -- and the getters say what it holds. A PDU of another
// format, or one that cannot be read, gives null, as Android's does.
//
// Why. Google Play services' SMS Retriever reads the messages of SMS_RECEIVED with SmsMessage (Telephony.Sms.Intents.
// getMessagesFromIntent), and megler.Phone sends that broadcast with the phone's messages: WhatsApp's code comes so.
// Everything else is the stub's, unchanged.
package android.telephony;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class SmsMessage {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int ENCODING_16BIT = 3;
    public static final int ENCODING_7BIT = 1;
    public static final int ENCODING_8BIT = 2;
    public static final int ENCODING_KSC5601 = 4;
    public static final int ENCODING_UNKNOWN = 0;
    public static final java.lang.String FORMAT_3GPP = "3gpp";
    public static final java.lang.String FORMAT_3GPP2 = "3gpp2";
    public static final int MAX_USER_DATA_BYTES = 140;
    public static final int MAX_USER_DATA_BYTES_WITH_HEADER = 134;
    public static final int MAX_USER_DATA_SEPTETS = 160;
    public static final int MAX_USER_DATA_SEPTETS_WITH_HEADER = 153;
    public static int[] calculateLength(java.lang.CharSequence p0, boolean p1) { return null; }
    public static int[] calculateLength(java.lang.CharSequence p0, boolean p1, int p2) { return null; }
    public static int[] calculateLength(java.lang.String p0, boolean p1) { return null; }
    public static int[] calculateLength(java.lang.String p0, boolean p1, int p2) { return null; }
    public static android.telephony.SmsMessage createFromEfRecord(int p0, byte[] p1) { return new SmsMessage(); }
    public static android.telephony.SmsMessage createFromEfRecord(int p0, byte[] p1, int p2) { return new SmsMessage(); }
    public static android.telephony.SmsMessage createFromNativeSmsSubmitPdu(byte[] p0, boolean p1) { return new SmsMessage(); }
    // ---- what a message holds (read by parse)
    private String mOriginating, mBody, mServiceCenter;
    private long mTimestamp;
    private int mProtocol, mEncoding, mStatusValue;
    private byte[] mPdu, mUserData;
    private boolean mReplyPath;
    private MessageClass mClass = MessageClass.UNKNOWN;

    /** A PDU as the phone's telephony hands it, 3GPP or 3GPP2; only 3GPP is read here. */
    public static android.telephony.SmsMessage createFromPdu(byte[] pdu) { return createFromPdu(pdu, FORMAT_3GPP); }
    public static android.telephony.SmsMessage createFromPdu(byte[] pdu, java.lang.String format) {
        if (pdu == null || (format != null && !FORMAT_3GPP.equals(format))) return null;
        try {
            SmsMessage m = new SmsMessage();
            m.parse(pdu);
            return m;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private void parse(byte[] p) {
        mPdu = p;
        int i = 0;
        int sca = p[i++] & 0xFF;
        if (sca > 0) {
            mServiceCenter = number(p, i + 1, (sca - 1) * 2, p[i] & 0xFF);
            i += sca;
        }
        int first = p[i++] & 0xFF;
        if ((first & 0x03) != 0) throw new IllegalArgumentException("not an SMS-DELIVER");
        mReplyPath = (first & 0x80) != 0;
        boolean header = (first & 0x40) != 0;
        int digits = p[i++] & 0xFF, type = p[i++] & 0xFF;
        if ((type & 0x70) == 0x50) mOriginating = megler.SmsCodec.unpack7(p, i, digits * 4 / 7, 0);
        else mOriginating = ((type & 0x70) == 0x10 ? "+" : "") + number(p, i, digits, type);
        i += (digits + 1) / 2;
        mProtocol = p[i++] & 0xFF;
        int dcs = p[i++] & 0xFF;
        mTimestamp = time(p, i);
        i += 7;
        int length = p[i++] & 0xFF;
        mEncoding = ENCODING_7BIT;
        if ((dcs & 0xC0) == 0) {
            int alphabet = (dcs >> 2) & 3;
            mEncoding = alphabet == 1 ? ENCODING_8BIT : alphabet == 2 ? ENCODING_16BIT : ENCODING_7BIT;
            if ((dcs & 0x10) != 0) mClass = MessageClass.values()[dcs & 3];
        } else if ((dcs & 0xF0) == 0xF0) {
            mEncoding = (dcs & 0x04) != 0 ? ENCODING_8BIT : ENCODING_7BIT;
            mClass = MessageClass.values()[dcs & 3];
        } else if ((dcs & 0xF0) == 0xE0) {
            mEncoding = ENCODING_16BIT;
        }
        int udh = header ? (p[i] & 0xFF) + 1 : 0;
        if (mEncoding == ENCODING_7BIT) {
            int fill = udh == 0 ? 0 : (7 - (udh * 8) % 7) % 7, skip = udh == 0 ? 0 : (udh * 8 + fill) / 7;
            mBody = megler.SmsCodec.unpack7(p, i + udh, length - skip, fill);
            mUserData = java.util.Arrays.copyOfRange(p, i, p.length);
        } else {
            int end = Math.min(p.length, i + length);
            mUserData = java.util.Arrays.copyOfRange(p, i + udh, end);
            if (mEncoding == ENCODING_16BIT) {
                StringBuilder b = new StringBuilder();
                for (int k = 0; k + 1 < mUserData.length; k += 2) b.append((char) ((mUserData[k] & 0xFF) << 8 | (mUserData[k + 1] & 0xFF)));
                mBody = b.toString();
            } else {
                mBody = new String(mUserData, java.nio.charset.StandardCharsets.ISO_8859_1);
            }
        }
    }

    /** A number in semi-octets, `digits` of them, as its type says it (a '+' is put by the caller). */
    private static String number(byte[] p, int at, int digits, int type) {
        StringBuilder b = new StringBuilder();
        for (int k = 0; k < digits; k++) {
            int v = p[at + k / 2] & 0xFF, d = (k & 1) == 0 ? v & 0xF : v >> 4;
            if (d == 0xF) break;
            b.append(d < 10 ? (char) ('0' + d) : d == 0xA ? '*' : d == 0xB ? '#' : d == 0xC ? 'a' : d == 0xD ? 'b' : 'c');
        }
        return b.toString();
    }

    /** The service centre's time stamp: seven swapped semi-octet pairs, the last the time zone in quarters of an hour. */
    private static long time(byte[] p, int at) {
        int[] v = new int[7];
        for (int k = 0; k < 7; k++) { int x = p[at + k] & 0xFF; v[k] = (x & (k == 6 ? 0x7 : 0xF)) * 10 + ((x >> 4) & 0xF); }
        java.util.Calendar c = java.util.Calendar.getInstance(java.util.TimeZone.getTimeZone("UTC"));
        c.clear();
        c.set(2000 + v[0] - (v[0] > 90 ? 100 : 0), v[1] - 1, v[2], v[3], v[4], v[5]);
        int zone = v[6] * 15 * 60 * 1000;
        if ((p[at + 6] & 0x08) != 0) zone = -zone;
        return c.getTimeInMillis() - zone;
    }

    public java.lang.String getDisplayMessageBody() { return mBody; }
    public java.lang.String getDisplayOriginatingAddress() { return mOriginating; }
    public java.lang.String getMessageBody() { return mBody; }
    public android.telephony.SmsMessage.MessageClass getMessageClass() { return mClass; }
    public java.lang.String getOriginatingAddress() { return mOriginating; }
    public byte[] getPdu() { return mPdu; }
    public int getProtocolIdentifier() { return mProtocol; }
    public int getReceivedEncodingType() { return mEncoding; }
    public java.lang.String getServiceCenterAddress() { return mServiceCenter; }
    public int getStatus() { return mStatusValue; }
    public long getTimestampMillis() { return mTimestamp; }
    public byte[] getUserData() { return mUserData; }
    public boolean is3gpp() { return true; }
    public boolean isReplyPathPresent() { return mReplyPath; }
    public static java.util.ArrayList fragmentText(java.lang.String p0) { return new java.util.ArrayList(); }
    public static java.util.ArrayList fragmentText(java.lang.String p0, int p1) { return new java.util.ArrayList(); }
    public static android.telephony.SmsMessage.SubmitPdu getSmsPdu(int p0, int p1, java.lang.String p2, java.lang.String p3, java.lang.String p4, long p5) { return null; }
    public static android.telephony.SmsMessage.SubmitPdu getSubmitPdu(java.lang.String p0, java.lang.String p1, java.lang.String p2, boolean p3) { return null; }
    public static android.telephony.SmsMessage.SubmitPdu getSubmitPdu(java.lang.String p0, java.lang.String p1, java.lang.String p2, boolean p3, int p4) { return null; }
    public static android.telephony.SmsMessage.SubmitPdu getSubmitPdu(java.lang.String p0, java.lang.String p1, short p2, byte[] p3, boolean p4) { return null; }
    public static byte[] getSubmitPduEncodedMessage(boolean p0, java.lang.String p1, java.lang.String p2, int p3, int p4, int p5, int p6, int p7, int p8) { return null; }
    public static int getTPLayerLengthForPDU(java.lang.String p0) { return 0; }
    public static boolean hasEmsSupport() { return false; }
    public static boolean shouldAppendPageNumberAsPrefix() { return false; }
    public java.lang.String getEmailBody() { return (java.lang.String) meglerProps.get("EmailBody"); }
    public java.lang.String getEmailFrom() { return (java.lang.String) meglerProps.get("EmailFrom"); }
    public int getIndexOnIcc() { return (meglerProps.get("IndexOnIcc") instanceof Integer ? (Integer) meglerProps.get("IndexOnIcc") : 0); }
    public int getIndexOnSim() { return (meglerProps.get("IndexOnSim") instanceof Integer ? (Integer) meglerProps.get("IndexOnSim") : 0); }
    public java.lang.String getPseudoSubject() { return (java.lang.String) meglerProps.get("PseudoSubject"); }
    public java.lang.String getRecipientAddress() { return (java.lang.String) meglerProps.get("RecipientAddress"); }
    public int getStatusOnIcc() { return (meglerProps.get("StatusOnIcc") instanceof Integer ? (Integer) meglerProps.get("StatusOnIcc") : 0); }
    public int getStatusOnSim() { return (meglerProps.get("StatusOnSim") instanceof Integer ? (Integer) meglerProps.get("StatusOnSim") : 0); }
    public int getSubId() { return (meglerProps.get("SubId") instanceof Integer ? (Integer) meglerProps.get("SubId") : 0); }
    public boolean isCphsMwiMessage() { return (meglerProps.get("CphsMwiMessage") instanceof Boolean ? (Boolean) meglerProps.get("CphsMwiMessage") : false); }
    public boolean isEmail() { return (meglerProps.get("Email") instanceof Boolean ? (Boolean) meglerProps.get("Email") : false); }
    public boolean isMWIClearMessage() { return (meglerProps.get("MWIClearMessage") instanceof Boolean ? (Boolean) meglerProps.get("MWIClearMessage") : false); }
    public boolean isMWISetMessage() { return (meglerProps.get("MWISetMessage") instanceof Boolean ? (Boolean) meglerProps.get("MWISetMessage") : false); }
    public boolean isMwiDontStore() { return (meglerProps.get("MwiDontStore") instanceof Boolean ? (Boolean) meglerProps.get("MwiDontStore") : false); }
    public boolean isReplace() { return (meglerProps.get("Replace") instanceof Boolean ? (Boolean) meglerProps.get("Replace") : false); }
    public boolean isStatusReportMessage() { return (meglerProps.get("StatusReportMessage") instanceof Boolean ? (Boolean) meglerProps.get("StatusReportMessage") : false); }
    public void setSubId(int p0) { meglerProps.put("SubId", Integer.valueOf(p0)); }
    public enum MessageClass {
        CLASS_0, CLASS_1, CLASS_2, CLASS_3, UNKNOWN;
    }
    public static class SubmitPdu {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public byte[] encodedMessage;
        public byte[] encodedScAddress;
    }
}
