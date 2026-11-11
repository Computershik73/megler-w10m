package android.icu.text;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step list-formatter calls of Android 13's class. */
public final class ListFormatter {
    public enum Type { AND, OR, UNITS }
    public enum Width { WIDE, SHORT, NARROW }
    private ListFormatter() {}
    public static ListFormatter getInstance(java.util.Locale locale) { return null; }
    public static ListFormatter getInstance(java.util.Locale locale, Type type, Width width) { return null; }
    public static ListFormatter getInstance(android.icu.util.ULocale locale, Type type, Width width) { return null; }
    public String format(Object... items) { return null; }
    public String format(java.util.Collection<?> items) { return null; }
}
