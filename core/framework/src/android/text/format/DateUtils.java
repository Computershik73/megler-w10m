package android.text.format;
public class DateUtils {
    public static final long SECOND_IN_MILLIS = 1000, MINUTE_IN_MILLIS = 60000, HOUR_IN_MILLIS = 3600000, DAY_IN_MILLIS = 86400000, WEEK_IN_MILLIS = 604800000, YEAR_IN_MILLIS = 31449600000L;
    public static final int FORMAT_SHOW_TIME = 1, FORMAT_SHOW_WEEKDAY = 2, FORMAT_SHOW_YEAR = 4, FORMAT_NO_YEAR = 8, FORMAT_SHOW_DATE = 16, FORMAT_NO_MONTH_DAY = 32, FORMAT_12HOUR = 64,
        FORMAT_24HOUR = 128, FORMAT_CAP_AMPM = 256, FORMAT_NO_NOON = 512, FORMAT_CAP_NOON = 1024, FORMAT_NO_MIDNIGHT = 2048, FORMAT_CAP_MIDNIGHT = 4096, FORMAT_UTC = 8192,
        FORMAT_ABBREV_TIME = 16384, FORMAT_ABBREV_WEEKDAY = 32768, FORMAT_ABBREV_MONTH = 65536, FORMAT_NUMERIC_DATE = 131072, FORMAT_ABBREV_RELATIVE = 262144, FORMAT_ABBREV_ALL = 524288;
    public static boolean isToday(long when) { java.util.Calendar a = java.util.Calendar.getInstance(), b = java.util.Calendar.getInstance(); b.setTimeInMillis(when); return a.get(java.util.Calendar.YEAR) == b.get(java.util.Calendar.YEAR) && a.get(java.util.Calendar.DAY_OF_YEAR) == b.get(java.util.Calendar.DAY_OF_YEAR); }
    public static CharSequence getRelativeTimeSpanString(long time) { return getRelativeTimeSpanString(time, System.currentTimeMillis(), MINUTE_IN_MILLIS); }
    public static CharSequence getRelativeTimeSpanString(long time, long now, long minRes) { return getRelativeTimeSpanString(time, now, minRes, 0); }
    public static CharSequence getRelativeTimeSpanString(long time, long now, long minRes, int flags) {
        long d = Math.abs(now - time); boolean past = time <= now;
        String r;
        if (d < MINUTE_IN_MILLIS && minRes < MINUTE_IN_MILLIS) r = (d / 1000) + " seconds";
        else if (d < HOUR_IN_MILLIS) r = (d / MINUTE_IN_MILLIS) + " minutes";
        else if (d < DAY_IN_MILLIS) r = (d / HOUR_IN_MILLIS) + " hours";
        else if (d < WEEK_IN_MILLIS) r = (d / DAY_IN_MILLIS) + " days";
        else return formatDateTime(null, time, FORMAT_SHOW_DATE);
        if (d < MINUTE_IN_MILLIS && minRes >= MINUTE_IN_MILLIS) return "Just now";
        return past ? r + " ago" : "In " + r;
    }
    public static CharSequence getRelativeTimeSpanString(android.content.Context c, long millis) { return getRelativeTimeSpanString(millis); }
    public static CharSequence getRelativeTimeSpanString(android.content.Context c, long millis, boolean withPreposition) { return getRelativeTimeSpanString(millis); }
    public static CharSequence getRelativeDateTimeString(android.content.Context c, long time, long minRes, long transRes, int flags) { return getRelativeTimeSpanString(time); }
    public static String formatDateTime(android.content.Context c, long millis, int flags) { return formatDateTime(c, millis, flags, null); }
    /* In the time zone named, or the default's where it is null; FORMAT_UTC's UTC whatever it names. */
    private static String formatDateTime(android.content.Context c, long millis, int flags, String timeZone) {
        StringBuilder p = new StringBuilder();
        if ((flags & FORMAT_SHOW_WEEKDAY) != 0) p.append((flags & (FORMAT_ABBREV_WEEKDAY | FORMAT_ABBREV_ALL)) != 0 ? "EEE, " : "EEEE, ");
        if ((flags & FORMAT_SHOW_DATE) != 0 || (flags & (FORMAT_SHOW_TIME)) == 0) { p.append((flags & FORMAT_NUMERIC_DATE) != 0 ? "M/d" : (flags & (FORMAT_ABBREV_MONTH | FORMAT_ABBREV_ALL)) != 0 ? "MMM d" : "MMMM d"); if ((flags & FORMAT_NO_YEAR) == 0 && (flags & FORMAT_SHOW_YEAR) != 0) p.append(", yyyy"); }
        if ((flags & FORMAT_SHOW_TIME) != 0) { if (p.length() > 0) p.append(", "); p.append((flags & FORMAT_12HOUR) != 0 ? "h:mm a" : "HH:mm"); }
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat(p.toString());
        if ((flags & FORMAT_UTC) != 0) f.setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
        else if (timeZone != null) f.setTimeZone(java.util.TimeZone.getTimeZone(timeZone));
        return f.format(new java.util.Date(millis));
    }
    public static String formatDateRange(android.content.Context c, long a, long b, int flags) { return range(c, a, b, flags, null); }
    public static java.util.Formatter formatDateRange(android.content.Context c, java.util.Formatter f, long a, long b, int flags) { return formatDateRange(c, f, a, b, flags, null); }
    /*
     * Megler: a range as Android's DateUtils formats it -- by libcore's DateIntervalFormat (ICU's interval formats, in Android
     * 9's core-libart), the clock of the user's when the flags name neither 12 nor 24 hours -- appended to the Formatter given,
     * which is answered. These two were stubs that answered null, and Transit's "Go" on a route took toString() of that and
     * ended (0.1.0.678). The plain format of formatDateTime where libcore's cannot be had.
     */
    public static java.util.Formatter formatDateRange(android.content.Context c, java.util.Formatter f, long a, long b, int flags, String timeZone) {
        try {
            f.out().append(range(c, a, b, flags, timeZone));
        } catch (java.io.IOException impossible) {
            throw new AssertionError(impossible);
        }
        return f;
    }
    private static java.lang.reflect.Method sIntervalFormat;
    private static boolean sIntervalTried;
    private static String range(android.content.Context c, long a, long b, int flags, String timeZone) {
        if ((flags & (FORMAT_SHOW_TIME | FORMAT_12HOUR | FORMAT_24HOUR)) == FORMAT_SHOW_TIME)
            flags |= DateFormat.is24HourFormat(c) ? FORMAT_24HOUR : FORMAT_12HOUR;
        try {
            synchronized (DateUtils.class) {
                if (!sIntervalTried) {
                    sIntervalTried = true;
                    sIntervalFormat = Class.forName("libcore.icu.DateIntervalFormat").getMethod("formatDateRange", long.class, long.class, int.class, String.class);
                }
            }
            if (sIntervalFormat != null) return (String) sIntervalFormat.invoke(null, a, b, flags, timeZone);
        } catch (Throwable t) {
            android.util.Log.w("DateUtils", "libcore's interval format: " + t);
        }
        String from = formatDateTime(c, a, flags, timeZone), to = formatDateTime(c, b, flags, timeZone);
        return from.equals(to) ? from : from + " – " + to;
    }
    public static String formatElapsedTime(long secs) { return formatElapsedTime(null, secs); }
    public static String formatElapsedTime(StringBuilder r, long secs) { long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60; String out = h > 0 ? String.format("%d:%02d:%02d", h, m, s) : String.format("%02d:%02d", m, s); if (r != null) { r.setLength(0); r.append(out); } return out; }
    public static CharSequence formatSameDayTime(long then, long now, int dateStyle, int timeStyle) { return isToday(then) ? java.text.DateFormat.getTimeInstance(timeStyle).format(new java.util.Date(then)) : java.text.DateFormat.getDateInstance(dateStyle).format(new java.util.Date(then)); }
    public static String getDayOfWeekString(int day, int abbrev) { return new java.text.DateFormatSymbols().getWeekdays()[day]; }
    public static String getMonthString(int month, int abbrev) { return new java.text.DateFormatSymbols().getMonths()[month]; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final java.lang.String ABBREV_MONTH_FORMAT = "%b";
    public static final java.lang.String ABBREV_WEEKDAY_FORMAT = "%a";
    public static final int FORMAT_CAP_NOON_MIDNIGHT = 5120;
    public static final int FORMAT_NO_NOON_MIDNIGHT = 2560;
    public static final java.lang.String HOUR_MINUTE_24 = "%H:%M";
    public static final int LENGTH_LONG = 10;
    public static final int LENGTH_MEDIUM = 20;
    public static final int LENGTH_SHORT = 30;
    public static final int LENGTH_SHORTER = 40;
    public static final int LENGTH_SHORTEST = 50;
    public static final java.lang.String MONTH_DAY_FORMAT = "%-d";
    public static final java.lang.String MONTH_FORMAT = "%B";
    public static final java.lang.String NUMERIC_MONTH_FORMAT = "%m";
    public static final java.lang.String WEEKDAY_FORMAT = "%A";
    public static final java.lang.String YEAR_FORMAT = "%Y";
    public static final java.lang.String YEAR_FORMAT_TWO_DIGITS = "%g";
    public static int[] sameMonthTable;
    public static int[] sameYearTable;
    /** Megler: Android's, a duration in hours, minutes or seconds as ICU's MeasureFormat words it (it answered null). */
    public static java.lang.CharSequence formatDuration(long millis) { return formatDuration(millis, LENGTH_LONG); }
    public static java.lang.CharSequence formatDuration(long millis, int abbrev) {
        final android.icu.text.MeasureFormat.FormatWidth width;
        switch (abbrev) {
            case LENGTH_LONG: width = android.icu.text.MeasureFormat.FormatWidth.WIDE; break;
            case LENGTH_SHORT: case LENGTH_SHORTER: case LENGTH_MEDIUM: width = android.icu.text.MeasureFormat.FormatWidth.SHORT; break;
            case LENGTH_SHORTEST: width = android.icu.text.MeasureFormat.FormatWidth.NARROW; break;
            default: width = android.icu.text.MeasureFormat.FormatWidth.WIDE;
        }
        final android.icu.text.MeasureFormat formatter = android.icu.text.MeasureFormat.getInstance(java.util.Locale.getDefault(), width);
        if (millis >= HOUR_IN_MILLIS) {
            return formatter.format(new android.icu.util.Measure((int) ((millis + 1800000) / HOUR_IN_MILLIS), android.icu.util.MeasureUnit.HOUR));
        } else if (millis >= MINUTE_IN_MILLIS) {
            return formatter.format(new android.icu.util.Measure((int) ((millis + 30000) / MINUTE_IN_MILLIS), android.icu.util.MeasureUnit.MINUTE));
        } else {
            return formatter.format(new android.icu.util.Measure((int) ((millis + 500) / SECOND_IN_MILLIS), android.icu.util.MeasureUnit.SECOND));
        }
    }
    public static java.lang.String getAMPMString(int p0) { return null; }
    // ---- end of generated members
}
