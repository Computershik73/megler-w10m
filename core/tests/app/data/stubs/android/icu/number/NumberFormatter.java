package android.icu.number;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step icu-number calls of Android 13's classes. */
public final class NumberFormatter {
    public enum UnitWidth { NARROW, SHORT, FULL_NAME, ISO_CODE, FORMAL, VARIANT, HIDDEN }
    public enum SignDisplay { AUTO, ALWAYS, NEVER, ACCOUNTING, ACCOUNTING_ALWAYS, EXCEPT_ZERO, ACCOUNTING_EXCEPT_ZERO, NEGATIVE, ACCOUNTING_NEGATIVE }
    public enum GroupingStrategy { OFF, MIN2, AUTO, ON_ALIGNED, THOUSANDS }
    public static UnlocalizedNumberFormatter with() { return null; }
    public static LocalizedNumberFormatter withLocale(java.util.Locale locale) { return null; }
}
