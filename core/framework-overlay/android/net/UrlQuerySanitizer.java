// Overlay of core/framework/gen-src/android/net/UrlQuerySanitizer.java (tools/build-framework.sh).
// It differs in reading a query: the parameters after a URL's '?', each name and value unescaped ("+" a space, "%41" the
// character), each value passed through the sanitizer registered for its name (or the one for unregistered names, when
// those are allowed; a parameter with no sanitizer is dropped), and what is left to be asked for by name or as the list
// in the query's order. The value sanitizers are Android's: characters a sanitizer's flags do not allow become '_' (a
// space when spaces are allowed), white space at the ends is cut when not all of it is allowed, and a javascript: or
// vbscript: value is emptied unless script URLs are allowed. The members and the constants are the stub's, all of them.
//
// Why. The generated class parsed nothing: getValue() was null for every parameter of every URL, and the nine ready
// sanitizers were null. An analytics library of HBO Max reads its app id so (new UrlQuerySanitizer(url).getValue("id")),
// and deep links are read with it.
package android.net;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class UrlQuerySanitizer {
    private final HashMap<String, ValueSanitizer> mSanitizers = new HashMap<>();
    private final HashMap<String, String> mEntries = new HashMap<>();
    private final ArrayList<ParameterValuePair> mEntriesList = new ArrayList<>();
    private boolean mAllowUnregisteredParamaters;
    private boolean mPreferFirstRepeatedParameter;
    private ValueSanitizer mUnregisteredParameterValueSanitizer = getAllIllegal();

    /** One parameter of the query as it was read: its name and its sanitized value. */
    public class ParameterValuePair {
        public String mParameter;
        public String mValue;
        public ParameterValuePair(String parameter, String value) {
            mParameter = parameter;
            mValue = value;
        }
    }

    public interface ValueSanitizer {
        String sanitize(String value);
    }

    /** A sanitizer by the characters it allows (the _OK flags); the others become '_', or a space where spaces are allowed. */
    public static class IllegalCharacterValueSanitizer implements ValueSanitizer {
        public static final int SPACE_OK = 1;
        public static final int OTHER_WHITESPACE_OK = 2;
        public static final int NON_7_BIT_ASCII_OK = 4;
        public static final int DQUOTE_OK = 8;
        public static final int SQUOTE_OK = 16;
        public static final int LT_OK = 32;
        public static final int GT_OK = 64;
        public static final int AMP_OK = 128;
        public static final int PCT_OK = 256;
        public static final int NUL_OK = 512;
        public static final int SCRIPT_URL_OK = 1024;
        public static final int ALL_OK = 2047;
        public static final int ALL_WHITESPACE_OK = 3;
        public static final int ALL_ILLEGAL = 0;
        public static final int ALL_BUT_NUL_LEGAL = 1535;
        public static final int ALL_BUT_WHITESPACE_LEGAL = 1532;
        public static final int URL_LEGAL = 404;
        public static final int URL_AND_SPACE_LEGAL = 405;
        public static final int AMP_LEGAL = 128;
        public static final int AMP_AND_SPACE_LEGAL = 129;
        public static final int SPACE_LEGAL = 1;
        public static final int ALL_BUT_NUL_AND_ANGLE_BRACKETS_LEGAL = 1439;

        private final int mFlags;

        public IllegalCharacterValueSanitizer(int flags) { mFlags = flags; }

        public String sanitize(String value) {
            if (value == null) return null;
            if ((mFlags & SCRIPT_URL_OK) == 0) {
                String lower = value.toLowerCase(Locale.ROOT);
                if (lower.startsWith("javascript:") || lower.startsWith("vbscript:")) return "";
            }
            if ((mFlags & ALL_WHITESPACE_OK) == 0) {
                int start = 0, end = value.length();
                while (start < end && isWhitespace(value.charAt(start))) start++;
                while (end > start && isWhitespace(value.charAt(end - 1))) end--;
                value = value.substring(start, end);
            }
            StringBuilder out = new StringBuilder(value.length());
            for (int i = 0; i < value.length(); i++) {
                char c = value.charAt(i);
                out.append(legal(c) ? c : (mFlags & SPACE_OK) != 0 ? ' ' : '_');
            }
            return out.toString();
        }

        private static boolean isWhitespace(char c) {
            return c == ' ' || c == '\t' || c == '\f' || c == '\n' || c == '\r' || c == 11;
        }

        private boolean legal(char c) {
            switch (c) {
                case ' ': return (mFlags & SPACE_OK) != 0;
                case '\t': case '\f': case '\n': case '\r': case 11: return (mFlags & OTHER_WHITESPACE_OK) != 0;
                case '"': return (mFlags & DQUOTE_OK) != 0;
                case '\'': return (mFlags & SQUOTE_OK) != 0;
                case '<': return (mFlags & LT_OK) != 0;
                case '>': return (mFlags & GT_OK) != 0;
                case '&': return (mFlags & AMP_OK) != 0;
                case '%': return (mFlags & PCT_OK) != 0;
                case '\0': return (mFlags & NUL_OK) != 0;
                default: return (c >= 32 && c < 127) || (c >= 128 && (mFlags & NON_7_BIT_ASCII_OK) != 0);
            }
        }
    }

    private static final ValueSanitizer sAllIllegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.ALL_ILLEGAL);
    private static final ValueSanitizer sAllButNulLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.ALL_BUT_NUL_LEGAL);
    private static final ValueSanitizer sAllButWhitespaceLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.ALL_BUT_WHITESPACE_LEGAL);
    private static final ValueSanitizer sUrlLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.URL_LEGAL);
    private static final ValueSanitizer sUrlAndSpaceLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.URL_AND_SPACE_LEGAL);
    private static final ValueSanitizer sAmpLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.AMP_LEGAL);
    private static final ValueSanitizer sAmpAndSpaceLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.AMP_AND_SPACE_LEGAL);
    private static final ValueSanitizer sSpaceLegal = new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.SPACE_LEGAL);
    private static final ValueSanitizer sAllButNulAndAngleBracketsLegal =
            new IllegalCharacterValueSanitizer(IllegalCharacterValueSanitizer.ALL_BUT_NUL_AND_ANGLE_BRACKETS_LEGAL);

    public static ValueSanitizer getAllIllegal() { return sAllIllegal; }
    public static ValueSanitizer getAllButNulLegal() { return sAllButNulLegal; }
    public static ValueSanitizer getAllButWhitespaceLegal() { return sAllButWhitespaceLegal; }
    public static ValueSanitizer getUrlLegal() { return sUrlLegal; }
    public static ValueSanitizer getUrlAndSpaceLegal() { return sUrlAndSpaceLegal; }
    public static ValueSanitizer getAmpLegal() { return sAmpLegal; }
    public static ValueSanitizer getAmpAndSpaceLegal() { return sAmpAndSpaceLegal; }
    public static ValueSanitizer getSpaceLegal() { return sSpaceLegal; }
    public static ValueSanitizer getAllButNulAndAngleBracketsLegal() { return sAllButNulAndAngleBracketsLegal; }

    /** A sanitizer with no parameters registered and unregistered ones not allowed: it keeps nothing until told what to. */
    public UrlQuerySanitizer() {}

    /**
     * The URL's query read at once, every parameter kept. In a value only printable ASCII stays, and of that not the space,
     * the quotes, the angle brackets, the ampersand and the percent sign: '_' for each of the others.
     */
    public UrlQuerySanitizer(String url) {
        setAllowUnregisteredParamaters(true);
        parseUrl(url);
    }

    public ValueSanitizer getUnregisteredParameterValueSanitizer() { return mUnregisteredParameterValueSanitizer; }
    public void setUnregisteredParameterValueSanitizer(ValueSanitizer sanitizer) { mUnregisteredParameterValueSanitizer = sanitizer; }
    public boolean getAllowUnregisteredParamaters() { return mAllowUnregisteredParamaters; }
    public void setAllowUnregisteredParamaters(boolean allow) { mAllowUnregisteredParamaters = allow; }
    public boolean getPreferFirstRepeatedParameter() { return mPreferFirstRepeatedParameter; }
    public void setPreferFirstRepeatedParameter(boolean preferFirst) { mPreferFirstRepeatedParameter = preferFirst; }

    /** What follows the URL's first '?' is the query; a URL with none has an empty one. */
    public void parseUrl(String url) {
        int at = url.indexOf('?');
        parseQuery(at >= 0 ? url.substring(at + 1) : "");
    }

    /** The query's parameters, split at '&', each at its first '=' (a parameter with none has the empty value). */
    public void parseQuery(String query) {
        clear();
        int from = 0, length = query.length();
        while (from <= length) {
            int to = query.indexOf('&', from);
            if (to < 0) to = length;
            if (to > from) {
                String pair = query.substring(from, to);
                int assign = pair.indexOf('=');
                if (assign < 0) parseEntry(pair, "");
                else parseEntry(pair.substring(0, assign), pair.substring(assign + 1));
            }
            from = to + 1;
        }
    }

    public Set<String> getParameterSet() { return mEntries.keySet(); }
    public List<ParameterValuePair> getParameterList() { return mEntriesList; }
    public boolean hasParameter(String parameter) { return mEntries.containsKey(parameter); }
    public String getValue(String parameter) { return mEntries.get(parameter); }

    /** The parameter's values go through this sanitizer; null takes the parameter's registration away. */
    public void registerParameter(String parameter, ValueSanitizer valueSanitizer) {
        if (valueSanitizer == null) mSanitizers.remove(parameter);
        else mSanitizers.put(parameter, valueSanitizer);
    }
    public void registerParameters(String[] parameters, ValueSanitizer valueSanitizer) {
        for (String parameter : parameters) mSanitizers.put(parameter, valueSanitizer);
    }

    public ValueSanitizer getValueSanitizer(String parameter) { return mSanitizers.get(parameter); }
    /** The parameter's own sanitizer, or the one for unregistered parameters when they are allowed; null drops it. */
    public ValueSanitizer getEffectiveValueSanitizer(String parameter) {
        ValueSanitizer sanitizer = getValueSanitizer(parameter);
        if (sanitizer == null && mAllowUnregisteredParamaters) sanitizer = getUnregisteredParameterValueSanitizer();
        return sanitizer;
    }

    protected void parseEntry(String parameter, String value) {
        String name = unescape(parameter);
        ValueSanitizer sanitizer = getEffectiveValueSanitizer(name);
        if (sanitizer == null) return;
        addSanitizedEntry(name, sanitizer.sanitize(unescape(value)));
    }

    /** Every entry is in the list; by name, the last of a repeated parameter wins unless the first is preferred. */
    protected void addSanitizedEntry(String parameter, String value) {
        mEntriesList.add(new ParameterValuePair(parameter, value));
        if (mPreferFirstRepeatedParameter && mEntries.containsKey(parameter)) return;
        mEntries.put(parameter, value);
    }

    protected void clear() {
        mEntries.clear();
        mEntriesList.clear();
    }

    /** A '+' is a space and "%" with two hex digits is that character; a '%' with anything else after it stays. */
    public String unescape(String string) {
        if (string.indexOf('%') < 0 && string.indexOf('+') < 0) return string;
        int length = string.length();
        StringBuilder out = new StringBuilder(length);
        for (int i = 0; i < length; i++) {
            char c = string.charAt(i);
            if (c == '+') c = ' ';
            else if (c == '%' && i + 2 < length && isHexDigit(string.charAt(i + 1)) && isHexDigit(string.charAt(i + 2))) {
                c = (char) (decodeHexDigit(string.charAt(i + 1)) * 16 + decodeHexDigit(string.charAt(i + 2)));
                i += 2;
            }
            out.append(c);
        }
        return out.toString();
    }

    protected boolean isHexDigit(char c) { return decodeHexDigit(c) >= 0; }

    /** The digit's value, -1 for a character that is not a hex digit. */
    protected int decodeHexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }
}
