package android.icu.util;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public final class ULocale implements java.io.Serializable, Comparable<ULocale> {
    private ULocale() {}
    public static ULocale getDefault() { return null; }
    public static ULocale forLocale(java.util.Locale locale) { return null; }
    public static ULocale addLikelySubtags(ULocale locale) { return null; }
    public java.util.Locale toLocale() { return null; }
    public String getCountry() { return null; }
    public String getKeywordValue(String keyword) { return null; }
    public ULocale setKeywordValue(String keyword, String value) { return null; }
    public String getUnicodeLocaleType(String key) { return null; }
    public int compareTo(ULocale other) { return 0; }
}
