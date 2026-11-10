package android.icu.text;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public class DateFormatSymbols implements java.io.Serializable, Cloneable {
    public static final int FORMAT = 0;
    public static final int STANDALONE = 1;
    public static final int ABBREVIATED = 0;
    public static final int WIDE = 1;
    public static final int NARROW = 2;
    public static final int SHORT = 3;
    public DateFormatSymbols(Class<? extends android.icu.util.Calendar> calendarClass, java.util.Locale locale) {}
    public String[] getAmPmStrings() { return null; }
    public String[] getMonths(int context, int width) { return null; }
    public String[] getWeekdays(int context, int width) { return null; }
}
