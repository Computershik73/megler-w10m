// megler-w10m overlay of Megler/src/java-framework/gen-src/android/telephony/PhoneNumberUtils.java (tools/build-framework.sh).
// It differs in answering: the original is the generated stub, whose every method said false, null or 0, so isNonSeparator('7')
// was false and an app that feeds a phone number's digits to its own formatter through it fed none (urent's mask then mapped
// one character of text onto none and threw out of Compose's offset mapping). Here, as Android's: the classes of characters,
// separators stripped, numbers normalised and keypad letters turned into digits, the network and post-dial portions, loose and
// strict comparison, the TOA, a URI's number, the standard emergency numbers of a phone with no SIM, and NANP formatting.
// Not here, as there is no libphonenumber in this framework: formatting for other countries (a number comes back as it was
// given) and E.164 outside NANP (null unless it is already international); nor the BCD codings of a SIM's records (null).
package android.telephony;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class PhoneNumberUtils {
    public static final int BCD_EXTENDED_TYPE_CALLED_PARTY = 2;
    public static final int BCD_EXTENDED_TYPE_EF_ADN = 1;
    public static final int FORMAT_JAPAN = 2;
    public static final int FORMAT_NANP = 1;
    public static final int FORMAT_UNKNOWN = 0;
    public static final char PAUSE = (char) 44;
    public static final int TOA_International = 145;
    public static final int TOA_Unknown = 129;
    public static final char WAIT = (char) 59;
    public static final char WILD = (char) 78;

    /** How many digits from the end two numbers must share to be the same number loosely (Android's default). */
    private static int sMinMatch = 7;
    /** The countries of the North American Numbering Plan, which share its format (Android's list). */
    private static final String[] NANP_COUNTRIES = { "US", "CA", "AS", "AI", "AG", "BS", "BB", "BM", "VG", "KY", "DM", "DO", "GD", "GU", "JM", "PR",
                                                     "MS", "MP", "KN", "LC", "VC", "TT", "TC", "VI" };
    /** The emergency numbers of a phone with no SIM (Android's ril.ecclist fallback). */
    private static final String[] EMERGENCY = { "112", "911", "000", "08", "110", "118", "119", "999" };
    private static final String CLIR_ON = "*31#", CLIR_OFF = "#31#";
    private static final String KEYPAD = "22233344455566677778889999";

    public PhoneNumberUtils() {}

    // ------------------------------------------------------------------ characters

    public static boolean isISODigit(char c) { return c >= '0' && c <= '9'; }
    public static boolean is12Key(char c) { return isISODigit(c) || c == '*' || c == '#'; }
    public static boolean isDialable(char c) { return is12Key(c) || c == '+' || c == WILD; }
    public static boolean isReallyDialable(char c) { return is12Key(c) || c == '+'; }
    public static boolean isNonSeparator(char c) { return isDialable(c) || c == WAIT || c == PAUSE; }
    public static boolean isStartsPostDial(char c) { return c == PAUSE || c == WAIT; }

    private static boolean isDialable(String s) {
        for (int i = 0; i < s.length(); i++)
            if (!isDialable(s.charAt(i))) return false;
        return true;
    }

    private static boolean isSeparator(char c) { return !isDialable(c) && !(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')); }

    // ------------------------------------------------------------------ stripping and normalising

    /** The number without what is not a digit, '*', '#', '+', WILD, WAIT or PAUSE; digits of other scripts as ASCII. */
    public static java.lang.String stripSeparators(java.lang.String number) {
        if (number == null) return null;
        StringBuilder b = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            int digit = Character.digit(c, 10);
            if (digit != -1) b.append(digit);
            else if (isNonSeparator(c)) b.append(c);
        }
        return b.toString();
    }

    public static java.lang.String convertKeypadLettersToDigits(java.lang.String input) {
        if (input == null) return null;
        char[] out = input.toCharArray();
        for (int i = 0; i < out.length; i++) {
            char c = out[i];
            if (c >= 'a' && c <= 'z') out[i] = KEYPAD.charAt(c - 'a');
            else if (c >= 'A' && c <= 'Z') out[i] = KEYPAD.charAt(c - 'A');
        }
        return new String(out);
    }

    public static java.lang.String convertAndStrip(java.lang.String number) { return stripSeparators(convertKeypadLettersToDigits(number)); }

    /** Digits only, a '+' kept where it begins the number, letters turned into the keypad's digits first. */
    public static java.lang.String normalizeNumber(java.lang.String number) {
        if (number == null || number.isEmpty()) return "";
        StringBuilder b = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            int digit = Character.digit(c, 10);
            if (digit != -1) b.append(digit);
            else if (b.length() == 0 && c == '+') b.append(c);
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return normalizeNumber(convertKeypadLettersToDigits(number));
        }
        return b.toString();
    }

    public static java.lang.String replaceUnicodeDigits(java.lang.String number) {
        if (number == null) return null;
        StringBuilder b = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            int digit = Character.digit(c, 10);
            b.append(digit != -1 ? (char) ('0' + digit) : c);
        }
        return b.toString();
    }

    /** 'p' and 'P' as PAUSE, 'w' and 'W' as WAIT. */
    public static java.lang.String convertPreDial(java.lang.String number) {
        if (number == null) return null;
        StringBuilder b = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            b.append(c == 'p' || c == 'P' ? PAUSE : c == 'w' || c == 'W' ? WAIT : c);
        }
        return b.toString();
    }

    // ------------------------------------------------------------------ portions

    /** What is dialled before the first PAUSE or WAIT: dialable characters, a '+' only at the start (or after a CLIR prefix). */
    public static java.lang.String extractNetworkPortion(java.lang.String number) {
        if (number == null) return null;
        StringBuilder b = new StringBuilder(number.length());
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            int digit = Character.digit(c, 10);
            if (digit != -1) {
                b.append(digit);
            } else if (c == '+') {
                String prefix = b.toString();
                if (prefix.isEmpty() || prefix.equals(CLIR_ON) || prefix.equals(CLIR_OFF)) b.append(c);
            } else if (isDialable(c)) {
                b.append(c);
            } else if (isStartsPostDial(c)) {
                break;
            }
        }
        return b.toString();
    }

    /** The same, with one '+' at most wherever it is first met. */
    public static java.lang.String extractNetworkPortionAlt(java.lang.String number) {
        if (number == null) return null;
        StringBuilder b = new StringBuilder(number.length());
        boolean plus = false;
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            if (c == '+') {
                if (plus) continue;
                plus = true;
            }
            if (isDialable(c)) b.append(c);
            else if (isStartsPostDial(c)) break;
        }
        return b.toString();
    }

    /** What follows the network portion: the non-separators from the first PAUSE or WAIT on. */
    public static java.lang.String extractPostDialPortion(java.lang.String number) {
        if (number == null) return null;
        int start = number.length();
        for (int i = 0; i < number.length(); i++)
            if (isStartsPostDial(number.charAt(i))) {
                start = i;
                break;
            }
        StringBuilder b = new StringBuilder();
        for (int i = start; i < number.length(); i++) {
            char c = number.charAt(i);
            if (isNonSeparator(c)) b.append(c);
        }
        return b.toString();
    }

    public static java.lang.String getStrippedReversed(java.lang.String number) {
        String np = extractNetworkPortionAlt(number);
        return np == null ? null : new StringBuilder(np).reverse().toString();
    }

    /** The last digits of the network portion, reversed, as many as a loose match needs. */
    public static java.lang.String toCallerIDMinMatch(java.lang.String number) {
        String np = extractNetworkPortionAlt(number);
        if (np == null) return null;
        String reversed = new StringBuilder(np).reverse().toString();
        return reversed.length() > sMinMatch ? reversed.substring(0, sMinMatch) : reversed;
    }

    public static int getMinMatchForTest() { return sMinMatch; }
    public static void setMinMatchForTest(int minMatch) { sMinMatch = minMatch; }

    // ------------------------------------------------------------------ comparing

    public static boolean compare(java.lang.String a, java.lang.String b) { return compare(a, b, false); }
    public static boolean compare(android.content.Context context, java.lang.String a, java.lang.String b) { return compare(a, b, false); }
    public static boolean compare(java.lang.String a, java.lang.String b, boolean strict) { return strict ? compareStrictly(a, b) : compareLoosely(a, b); }

    /** The same number when their dialable digits agree from the end for the minimum match, or wholly when either is shorter. */
    public static boolean compareLoosely(java.lang.String a, java.lang.String b) {
        if (a == null || b == null) return a == b;
        if (a.isEmpty() || b.isEmpty()) return false;
        String x = digits(a), y = digits(b);
        if (x.length() < sMinMatch || y.length() < sMinMatch) return x.equals(y) && !x.isEmpty();
        return x.regionMatches(x.length() - sMinMatch, y, y.length() - sMinMatch, sMinMatch);
    }

    public static boolean compareStrictly(java.lang.String a, java.lang.String b) { return compareStrictly(a, b, true); }

    /** The same number when their normalised forms agree, a country's '+' and code allowed on one side for a national number on the other. */
    public static boolean compareStrictly(java.lang.String a, java.lang.String b, boolean acceptInvalidCcNdc) {
        if (a == null || b == null) return a == b;
        if (a.isEmpty() && b.isEmpty()) return false;
        String x = normalizeNumber(extractNetworkPortion(a)), y = normalizeNumber(extractNetworkPortion(b));
        if (x.equals(y)) return true;
        if (x.startsWith("+") == y.startsWith("+")) return false;
        String international = x.startsWith("+") ? x : y, national = x.startsWith("+") ? y : x;
        if (national.startsWith("0")) national = national.substring(1);
        // A country code is one to three digits.
        for (int cc = 1; cc <= 3 && cc < international.length(); cc++)
            if (international.substring(1 + cc).equals(national)) return true;
        return false;
    }

    public static boolean areSamePhoneNumber(java.lang.String a, java.lang.String b, java.lang.String defaultCountryIso) { return compareStrictly(a, b, true); }

    private static String digits(String number) {
        StringBuilder b = new StringBuilder(number.length());
        String np = extractNetworkPortionAlt(number);
        for (int i = 0; i < np.length(); i++) {
            char c = np.charAt(i);
            if (c != '+' && isDialable(c)) b.append(c);
        }
        return b.toString();
    }

    // ------------------------------------------------------------------ what a number is

    public static boolean isGlobalPhoneNumber(java.lang.String number) { return number != null && !number.isEmpty() && number.matches("[\\+]?[0-9.-]+"); }

    public static boolean isWellFormedSmsAddress(java.lang.String address) {
        String np = extractNetworkPortion(address);
        return np != null && !np.isEmpty() && !np.equals("+") && isDialable(np);
    }

    public static boolean isUriNumber(java.lang.String number) { return number != null && (number.contains("@") || number.contains("%40")); }

    public static java.lang.String getUsernameFromUriNumber(java.lang.String number) {
        int at = number.indexOf('@');
        if (at < 0) at = number.indexOf("%40");
        return at < 0 ? number : number.substring(0, at);
    }

    /** Ten digits whose area code and exchange do not begin with 0 or 1. */
    public static boolean isNanp(java.lang.String number) {
        if (number == null || number.length() != 10 || !isTwoToNine(number.charAt(0)) || !isTwoToNine(number.charAt(3))) return false;
        for (int i = 0; i < 10; i++)
            if (!isISODigit(number.charAt(i))) return false;
        return true;
    }

    private static boolean isTwoToNine(char c) { return c >= '2' && c <= '9'; }

    public static boolean isInternationalNumber(java.lang.String number, java.lang.String defaultCountryIso) {
        if (number == null) return false;
        String n = normalizeNumber(number);
        return n.startsWith("+") || n.startsWith("00") || (isNanpCountry(defaultCountryIso) && n.startsWith("011"));
    }

    public static boolean isEmergencyNumber(java.lang.String number) {
        if (number == null || isUriNumber(number)) return false;
        String n = extractNetworkPortionAlt(number);
        for (String e : EMERGENCY)
            if (e.equals(n)) return true;
        return false;
    }

    public static boolean isEmergencyNumber(int subId, java.lang.String number) { return isEmergencyNumber(number); }
    public static boolean isLocalEmergencyNumber(android.content.Context context, java.lang.String number) { return isEmergencyNumber(number); }
    public static java.lang.String convertToEmergencyNumber(android.content.Context context, java.lang.String number) { return number; }

    /** No SIM, so no voicemail number. */
    public static boolean isVoiceMailNumber(java.lang.String number) { return false; }
    public static boolean isVoiceMailNumber(int subId, java.lang.String number) { return false; }
    public static boolean isVoiceMailNumber(android.content.Context context, int subId, java.lang.String number) { return false; }

    public static boolean isWpsCallNumber(java.lang.String number) { return number != null && (number.startsWith("*272") || number.startsWith("*31#*272") || number.startsWith("#31#*272")); }

    // ------------------------------------------------------------------ TOA, intents, URIs

    public static int toaFromString(java.lang.String number) { return number != null && number.startsWith("+") ? TOA_International : TOA_Unknown; }

    public static java.lang.String stringFromStringAndTOA(java.lang.String number, int toa) {
        if (number == null) return null;
        return toa == TOA_International && !number.startsWith("+") ? "+" + number : number;
    }

    /** The number of a tel: or sip: intent (Android also looks a contact up by its data's URI; there are no contacts here). */
    public static java.lang.String getNumberFromIntent(android.content.Intent intent, android.content.Context context) {
        android.net.Uri uri = intent == null ? null : intent.getData();
        if (uri == null) return null;
        String scheme = uri.getScheme();
        return "tel".equals(scheme) || "sip".equals(scheme) || "voicemail".equals(scheme) ? uri.getSchemeSpecificPart() : null;
    }

    public static android.net.Uri convertSipUriToTelUri(android.net.Uri uri) {
        if (uri == null || !"sip".equals(uri.getScheme())) return uri;
        String number = getUsernameFromUriNumber(uri.getSchemeSpecificPart());
        return android.net.Uri.fromParts("tel", number, null);
    }

    // ------------------------------------------------------------------ formatting

    public static int getFormatTypeForLocale(java.util.Locale locale) {
        String country = locale == null ? "" : locale.getCountry();
        if (isNanpCountry(country)) return FORMAT_NANP;
        return "JP".equalsIgnoreCase(country) ? FORMAT_JAPAN : FORMAT_UNKNOWN;
    }

    private static boolean isNanpCountry(String iso) {
        if (iso == null) return false;
        for (String c : NANP_COUNTRIES)
            if (c.equalsIgnoreCase(iso)) return true;
        return false;
    }

    public static java.lang.String formatNumber(java.lang.String source) { return formatNumber(source, getFormatTypeForLocale(java.util.Locale.getDefault())); }

    public static java.lang.String formatNumber(java.lang.String source, int defaultFormattingType) {
        if (source == null) return null;
        if (defaultFormattingType != FORMAT_NANP) return source;
        android.text.SpannableStringBuilder text = new android.text.SpannableStringBuilder(source);
        formatNanpNumber(text);
        return text.toString();
    }

    /** NANP numbers formatted (+1 or 1, then 3-3-4); any other country's as given, there being no libphonenumber here. */
    public static java.lang.String formatNumber(java.lang.String phoneNumber, java.lang.String defaultCountryIso) {
        if (phoneNumber == null || phoneNumber.startsWith("#") || phoneNumber.startsWith("*")) return phoneNumber;
        String n = normalizeNumber(phoneNumber);
        if (n.startsWith("+1") || (!n.startsWith("+") && isNanpCountry(defaultCountryIso))) return formatNumber(phoneNumber, FORMAT_NANP);
        return phoneNumber;
    }

    public static java.lang.String formatNumber(java.lang.String phoneNumber, java.lang.String phoneNumberE164, java.lang.String defaultCountryIso) {
        return formatNumber(phoneNumber, defaultCountryIso);
    }

    public static void formatNumber(android.text.Editable text, int defaultFormattingType) {
        if (defaultFormattingType == FORMAT_NANP) formatNanpNumber(text);
    }

    /** The NANP's dashes: 1-xxx-xxx-xxxx, xxx-xxx-xxxx or xxx-xxxx, '+1' kept; anything else left as it is. */
    public static void formatNanpNumber(android.text.Editable text) {
        String s = text.toString();
        if (s.length() > "+1-nnn-nnn-nnnn".length() || s.startsWith("*") || s.startsWith("#")) return;
        StringBuilder digits = new StringBuilder();
        boolean plus = false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (isISODigit(c)) digits.append(c);
            else if (c == '+' && i == 0) plus = true;
            else if (!isSeparator(c)) return;
        }
        String d = digits.toString();
        String out;
        if (d.length() == 11 && d.charAt(0) == '1') out = (plus ? "+" : "") + "1-" + d.substring(1, 4) + "-" + d.substring(4, 7) + "-" + d.substring(7);
        else if (!plus && d.length() == 10) out = d.substring(0, 3) + "-" + d.substring(3, 6) + "-" + d.substring(6);
        else if (!plus && d.length() == 7) out = d.substring(0, 3) + "-" + d.substring(3);
        else return;
        text.replace(0, text.length(), out);
    }

    /** Japan's grouping needs its numbering plan's tables; left as typed. */
    public static void formatJapaneseNumber(android.text.Editable text) {}

    /** E.164: a number already international, normalised; a NANP number in a NANP country, with +1; otherwise null. */
    public static java.lang.String formatNumberToE164(java.lang.String phoneNumber, java.lang.String defaultCountryIso) {
        if (phoneNumber == null) return null;
        String n = normalizeNumber(phoneNumber);
        if (n.startsWith("+")) return n.length() > 1 ? n : null;
        if (isNanpCountry(defaultCountryIso)) {
            if (n.length() == 11 && n.charAt(0) == '1') return "+" + n;
            if (n.length() == 10) return "+1" + n;
        }
        return null;
    }

    public static java.lang.String formatNumberToRFC3966(java.lang.String phoneNumber, java.lang.String defaultCountryIso) {
        String e164 = formatNumberToE164(phoneNumber, defaultCountryIso);
        return e164 == null ? null : "tel:" + e164;
    }

    // ------------------------------------------------------------------ CDMA's plus code: no CDMA network, so a number stays as it is

    public static java.lang.String cdmaCheckAndProcessPlusCode(java.lang.String dialStr) { return dialStr; }
    public static java.lang.String cdmaCheckAndProcessPlusCodeForSms(java.lang.String dialStr) { return dialStr; }
    public static java.lang.String cdmaCheckAndProcessPlusCodeByNumberFormat(java.lang.String dialStr, int currFormat, int defaultFormat) { return dialStr; }

    // ------------------------------------------------------------------ text to speech: the text as it is

    public static void addTtsSpan(android.text.Spannable s, int start, int end) {}
    public static android.text.style.TtsSpan createTtsSpan(java.lang.String phoneNumberString) { return null; }
    public static java.lang.CharSequence createTtsSpannable(java.lang.CharSequence phoneNumber) { return phoneNumber; }
    public static java.lang.CharSequence ttsSpanAsPhoneNumber(java.lang.CharSequence phoneNumber) { return phoneNumber; }
    public static void ttsSpanAsPhoneNumber(android.text.Spannable s, int start, int end) {}

    // ------------------------------------------------------------------ a SIM's BCD records: there is no SIM

    public static java.lang.String calledPartyBCDFragmentToString(byte[] bytes, int offset, int length) { return null; }
    public static java.lang.String calledPartyBCDFragmentToString(byte[] bytes, int offset, int length, int bcdExtType) { return null; }
    public static java.lang.String calledPartyBCDToString(byte[] bytes, int offset, int length) { return null; }
    public static java.lang.String calledPartyBCDToString(byte[] bytes, int offset, int length, int bcdExtType) { return null; }
    public static byte[] networkPortionToCalledPartyBCD(java.lang.String s) { return null; }
    public static byte[] networkPortionToCalledPartyBCDWithLength(java.lang.String s) { return null; }
    public static byte[] numberToCalledPartyBCD(java.lang.String number) { return null; }
    public static byte[] numberToCalledPartyBCD(java.lang.String number, int bcdExtType) { return null; }
}
