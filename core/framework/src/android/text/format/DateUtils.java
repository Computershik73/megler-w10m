package android.text.format;
public class DateUtils {
    public static final long SECOND_IN_MILLIS = 1000, MINUTE_IN_MILLIS = 60000, HOUR_IN_MILLIS = 3600000, DAY_IN_MILLIS = 86400000, WEEK_IN_MILLIS = 604800000, YEAR_IN_MILLIS = 31449600000L;
    public static final int FORMAT_SHOW_TIME = 1, FORMAT_SHOW_WEEKDAY = 2, FORMAT_SHOW_YEAR = 4, FORMAT_NO_YEAR = 8, FORMAT_SHOW_DATE = 16, FORMAT_NO_MONTH_DAY = 32, FORMAT_12HOUR = 64,
        FORMAT_24HOUR = 128, FORMAT_CAP_AMPM = 256, FORMAT_NO_NOON = 512, FORMAT_CAP_NOON = 1024, FORMAT_NO_MIDNIGHT = 2048, FORMAT_CAP_MIDNIGHT = 4096, FORMAT_UTC = 8192,
        FORMAT_ABBREV_TIME = 16384, FORMAT_ABBREV_WEEKDAY = 32768, FORMAT_ABBREV_MONTH = 65536, FORMAT_NUMERIC_DATE = 131072, FORMAT_ABBREV_RELATIVE = 262144, FORMAT_ABBREV_ALL = 524288;
    public static boolean isToday(long when) { java.util.Calendar a = java.util.Calendar.getInstance(), b = java.util.Calendar.getInstance(); b.setTimeInMillis(when); return a.get(java.util.Calendar.YEAR) == b.get(java.util.Calendar.YEAR) && a.get(java.util.Calendar.DAY_OF_YEAR) == b.get(java.util.Calendar.DAY_OF_YEAR); }
    /*
     * Megler: the relative times as Android's DateUtils words them -- libcore's RelativeDateTimeFormatter (Android 9's
     * core-libart: ICU's RelativeDateTimeFormatter and DateIntervalFormat's skeletons, in the default locale and zone) by
     * reflection, as range() reaches DateIntervalFormat -- and formatDateTime as Android's, formatDateRange of the instant
     * alone. They answered English ("5 minutes ago") and an English order of the locale's names ("May 20, 2023" in Russian
     * as "мая 20, 2023"). The plain English below is where libcore's cannot be had.
     */
    public static CharSequence getRelativeTimeSpanString(long startTime) { return getRelativeTimeSpanString(startTime, System.currentTimeMillis(), MINUTE_IN_MILLIS); }
    public static CharSequence getRelativeTimeSpanString(long time, long now, long minResolution) {
        return getRelativeTimeSpanString(time, now, minResolution, FORMAT_SHOW_DATE | FORMAT_SHOW_YEAR | FORMAT_ABBREV_MONTH);
    }
    public static CharSequence getRelativeTimeSpanString(long time, long now, long minResolution, int flags) {
        final Object said = libcoreRelative("getRelativeTimeSpanString", new Class<?>[] { java.util.Locale.class, java.util.TimeZone.class, long.class, long.class, long.class, int.class },
                java.util.Locale.getDefault(), java.util.TimeZone.getDefault(), time, now, minResolution, flags);
        if (said != null) return (String) said;
        return plainRelativeTimeSpan(time, now, minResolution, flags);
    }
    private static CharSequence plainRelativeTimeSpan(long time, long now, long minRes, int flags) {
        long d = Math.abs(now - time); boolean past = time <= now;
        String r;
        if (d < MINUTE_IN_MILLIS && minRes < MINUTE_IN_MILLIS) r = (d / 1000) + " seconds";
        else if (d < HOUR_IN_MILLIS) r = (d / MINUTE_IN_MILLIS) + " minutes";
        else if (d < DAY_IN_MILLIS) r = (d / HOUR_IN_MILLIS) + " hours";
        else if (d < WEEK_IN_MILLIS) r = (d / DAY_IN_MILLIS) + " days";
        else return plainDateTime(null, time, FORMAT_SHOW_DATE | (flags & (FORMAT_SHOW_YEAR | FORMAT_NO_YEAR | FORMAT_ABBREV_MONTH | FORMAT_NUMERIC_DATE)), null);
        if (d < MINUTE_IN_MILLIS && minRes >= MINUTE_IN_MILLIS) return "Just now";
        return past ? r + " ago" : "In " + r;
    }
    public static CharSequence getRelativeTimeSpanString(android.content.Context c, long millis) { return getRelativeTimeSpanString(c, millis, false); }
    /* Android's: the time today, the date with the year of another year, the month and day of this one; "at"/"on" from the platform's strings. */
    public static CharSequence getRelativeTimeSpanString(android.content.Context c, long millis, boolean withPreposition) {
        final long now = System.currentTimeMillis();
        final long span = Math.abs(now - millis);
        final java.util.Calendar nowCal = java.util.Calendar.getInstance(), thenCal = java.util.Calendar.getInstance();
        nowCal.setTimeInMillis(now);
        thenCal.setTimeInMillis(millis);
        String result;
        boolean time;
        if (span < DAY_IN_MILLIS && nowCal.get(java.util.Calendar.DAY_OF_WEEK) == thenCal.get(java.util.Calendar.DAY_OF_WEEK)) {
            result = formatDateRange(c, millis, millis, FORMAT_SHOW_TIME);
            time = true;
        } else if (nowCal.get(java.util.Calendar.YEAR) != thenCal.get(java.util.Calendar.YEAR)) {
            result = formatDateRange(c, millis, millis, FORMAT_SHOW_DATE | FORMAT_SHOW_YEAR | FORMAT_NUMERIC_DATE);
            time = false;
        } else {
            result = formatDateRange(c, millis, millis, FORMAT_SHOW_DATE | FORMAT_ABBREV_MONTH);
            time = false;
        }
        if (withPreposition && c != null) {
            // The platform's preposition_for_time or _date in the context's language, found by name: megler.IR's numbers are another release's.
            try {
                final android.content.res.Resources res = c.getResources();
                final int id = res.getIdentifier(time ? "preposition_for_time" : "preposition_for_date", "string", "android");
                result = id != 0 ? res.getString(id, result) : (time ? "at " : "on ") + result;
            } catch (RuntimeException e) {
                result = (time ? "at " : "on ") + result;
            }
        }
        return result;
    }
    public static CharSequence getRelativeDateTimeString(android.content.Context c, long time, long minResolution, long transitionResolution, int flags) {
        // The user's clock where the flags name neither 12 nor 24 hours, as for a range.
        if ((flags & (FORMAT_SHOW_TIME | FORMAT_12HOUR | FORMAT_24HOUR)) == FORMAT_SHOW_TIME)
            flags |= DateFormat.is24HourFormat(c) ? FORMAT_24HOUR : FORMAT_12HOUR;
        final Object said = libcoreRelative("getRelativeDateTimeString",
                new Class<?>[] { java.util.Locale.class, java.util.TimeZone.class, long.class, long.class, long.class, long.class, int.class },
                java.util.Locale.getDefault(), java.util.TimeZone.getDefault(), time, System.currentTimeMillis(), minResolution, transitionResolution, flags);
        if (said != null) return (String) said;
        return plainRelativeTimeSpan(time, System.currentTimeMillis(), minResolution, flags) + ", " + plainDateTime(c, time, FORMAT_SHOW_TIME | (flags & (FORMAT_12HOUR | FORMAT_24HOUR)), null);
    }
    private static final java.util.HashMap<String, java.lang.reflect.Method> sRelative = new java.util.HashMap<>();
    private static boolean sRelativeTried;
    /* libcore.icu.RelativeDateTimeFormatter's method of that name, or null where it cannot be had (said once). */
    private static Object libcoreRelative(String name, Class<?>[] types, Object... args) {
        java.lang.reflect.Method m;
        synchronized (sRelative) {
            if (!sRelativeTried) {
                sRelativeTried = true;
                try {
                    Class<?> k = Class.forName("libcore.icu.RelativeDateTimeFormatter");
                    sRelative.put("getRelativeTimeSpanString", k.getMethod("getRelativeTimeSpanString", java.util.Locale.class, java.util.TimeZone.class, long.class, long.class, long.class, int.class));
                    sRelative.put("getRelativeDateTimeString", k.getMethod("getRelativeDateTimeString", java.util.Locale.class, java.util.TimeZone.class, long.class, long.class, long.class, long.class, int.class));
                } catch (Throwable t) {
                    android.util.Log.w("DateUtils", "libcore's relative time format: " + t);
                }
            }
            m = sRelative.get(name);
        }
        if (m == null) return null;
        try {
            return m.invoke(null, args);
        } catch (Throwable t) {
            android.util.Log.w("DateUtils", "libcore's relative time format: " + (t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null ? t.getCause() : t));
            return null;
        }
    }
    public static String formatDateTime(android.content.Context c, long millis, int flags) { return formatDateRange(c, millis, millis, flags); }
    /* The plain format where libcore's cannot be had: in the time zone named, or the default's where it is null; FORMAT_UTC's UTC whatever it names. */
    private static String plainDateTime(android.content.Context c, long millis, int flags, String timeZone) {
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
        String from = plainDateTime(c, a, flags, timeZone), to = plainDateTime(c, b, flags, timeZone);
        return from.equals(to) ? from : from + " – " + to;
    }
    public static String formatElapsedTime(long secs) { return formatElapsedTime(null, secs); }
    public static String formatElapsedTime(StringBuilder r, long secs) { long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60; String out = h > 0 ? String.format("%d:%02d:%02d", h, m, s) : String.format("%02d:%02d", m, s); if (r != null) { r.setLength(0); r.append(out); } return out; }
    /* The time where `then` is on the day of `now` (the day of today's clock it was), else the date. */
    public static CharSequence formatSameDayTime(long then, long now, int dateStyle, int timeStyle) {
        final java.util.Calendar thenCal = new java.util.GregorianCalendar(), nowCal = new java.util.GregorianCalendar();
        thenCal.setTimeInMillis(then);
        nowCal.setTimeInMillis(now);
        final boolean sameDay = thenCal.get(java.util.Calendar.YEAR) == nowCal.get(java.util.Calendar.YEAR) && thenCal.get(java.util.Calendar.MONTH) == nowCal.get(java.util.Calendar.MONTH)
                && thenCal.get(java.util.Calendar.DAY_OF_MONTH) == nowCal.get(java.util.Calendar.DAY_OF_MONTH);
        return (sameDay ? java.text.DateFormat.getTimeInstance(timeStyle) : java.text.DateFormat.getDateInstance(dateStyle)).format(thenCal.getTime());
    }
    /* Android's: the default locale's names in ICU's widths (LENGTH_LONG the whole name, LENGTH_SHORTEST the narrow one, the others abbreviated). */
    public static String getDayOfWeekString(int dayOfWeek, int abbrev) {
        final int width = abbrev == LENGTH_LONG ? android.icu.text.DateFormatSymbols.WIDE : abbrev == LENGTH_SHORTEST ? android.icu.text.DateFormatSymbols.NARROW : android.icu.text.DateFormatSymbols.ABBREVIATED;
        return DateFormat.getIcuDateFormatSymbols(java.util.Locale.getDefault()).getWeekdays(android.icu.text.DateFormatSymbols.FORMAT, width)[dayOfWeek];
    }
    public static String getMonthString(int month, int abbrev) {
        final int width = abbrev == LENGTH_LONG ? android.icu.text.DateFormatSymbols.WIDE : abbrev == LENGTH_SHORTEST ? android.icu.text.DateFormatSymbols.NARROW : android.icu.text.DateFormatSymbols.ABBREVIATED;
        return DateFormat.getIcuDateFormatSymbols(java.util.Locale.getDefault()).getMonths(android.icu.text.DateFormatSymbols.FORMAT, width)[month];
    }
    public static String getAMPMString(int ampm) {
        return DateFormat.getIcuDateFormatSymbols(java.util.Locale.getDefault()).getAmPmStrings()[ampm - java.util.Calendar.AM];
    }
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
    /*
     * Megler: Android's, a duration in hours, minutes or seconds as ICU's MeasureFormat words it (they answered null) -- ICU's
     * of core-libart by reflection (the SDK's stubs the framework is built against have not those calls), plain English where
     * it cannot be had.
     */
    public static java.lang.CharSequence formatDuration(long millis) { return formatDuration(millis, LENGTH_LONG); }
    public static java.lang.CharSequence formatDuration(long millis, int abbrev) {
        final String unit;
        final int count;
        if (millis >= HOUR_IN_MILLIS) { unit = "HOUR"; count = (int) ((millis + 1800000) / HOUR_IN_MILLIS); }
        else if (millis >= MINUTE_IN_MILLIS) { unit = "MINUTE"; count = (int) ((millis + 30000) / MINUTE_IN_MILLIS); }
        else { unit = "SECOND"; count = (int) ((millis + 500) / SECOND_IN_MILLIS); }
        final String width = abbrev == LENGTH_LONG ? "WIDE" : abbrev == LENGTH_SHORTEST ? "NARROW"
                : (abbrev == LENGTH_SHORT || abbrev == LENGTH_SHORTER || abbrev == LENGTH_MEDIUM) ? "SHORT" : "WIDE";
        try {
            Class<?> widthClass = Class.forName("android.icu.text.MeasureFormat$FormatWidth");
            Class<?> formatClass = Class.forName("android.icu.text.MeasureFormat");
            Class<?> unitClass = Class.forName("android.icu.util.MeasureUnit");
            Object formatter = formatClass.getMethod("getInstance", java.util.Locale.class, widthClass)
                    .invoke(null, java.util.Locale.getDefault(), widthClass.getField(width).get(null));
            Object measure = Class.forName("android.icu.util.Measure").getConstructor(Number.class, unitClass)
                    .newInstance(count, unitClass.getField(unit).get(null));
            return (String) formatClass.getMethod("format", Object.class).invoke(formatter, measure);
        } catch (Throwable t) {
            String word = unit.toLowerCase(java.util.Locale.ROOT);
            if (!"WIDE".equals(width)) word = "HOUR".equals(unit) ? "hr" : "MINUTE".equals(unit) ? "min" : "sec";
            return count + " " + word + ("WIDE".equals(width) && count != 1 ? "s" : "");
        }
    }
    // ---- end of generated members
}
