package android.text.format;

import android.content.Context;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.SpannedString;
import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.GregorianCalendar;
import java.util.Locale;
import java.util.TimeZone;

/*
 * Megler: Android's own, as Android 10 and later have it (it was a stub that answered the skeleton itself for a pattern, 24
 * hours always, d, M, y for every order, and format() through SimpleDateFormat, which knows none of this class's letters).
 *
 * - A pattern for a skeleton is ICU's DateTimePatternGenerator's best (core-libart's android.icu, ICU 60's data: what Android
 *   9's libcore.icu.ICU.getBestDateTimePattern found through ICU4C), the hour's length as the skeleton asks
 *   (MATCH_HOUR_FIELD_LENGTH). Messenger formats its chat list's older dates so ("MMMd", "MMMd, yyyy" on a
 *   java.text.SimpleDateFormat): with the skeleton for a pattern a Russian phone read "мая20, 2023" where Android says
 *   "20 мая 2023 г.".
 * - 12 or 24 hours: Settings.System.TIME_12_24 where it is set, the locale's own where it is not (its long time pattern
 *   has an 'H'), as Android's is24HourFormat; the time pattern is the best for "Hm" or "hm" with the hour as long as the
 *   locale has it, as Android 12's ("H:mm" in Russian with ICU 60's data, "HH:mm" in American English).
 * - format(): Android's own letters (a/A, c/E, d, h/K, H/k, L/M, m, s, y, z), quoting, the names ICU's for the default
 *   locale (standalone and narrow forms included), a Spanned given a SpannedString back.
 * Where ICU cannot be had, a pattern is the skeleton as before.
 */
public class DateFormat {
    public static final char QUOTE = '\'';
    public static final char AM_PM = 'a';
    public static final char CAPITAL_AM_PM = 'A';
    public static final char DATE = 'd';
    public static final char DAY = 'E';
    public static final char HOUR = 'h';
    public static final char HOUR_OF_DAY = 'k';
    public static final char MINUTE = 'm';
    public static final char MONTH = 'M';
    public static final char STANDALONE_MONTH = 'L';
    public static final char SECONDS = 's';
    public static final char TIME_ZONE = 'z';
    public static final char YEAR = 'y';

    private static final Object sLocaleLock = new Object();
    private static Locale sIs24HourLocale;
    private static boolean sIs24Hour;

    public DateFormat() {}

    /** The locale a context's resources are in; the default where there is no context. */
    private static Locale localeOf(Context context) {
        Locale locale = null;
        if (context != null) {
            try {
                locale = context.getResources().getConfiguration().locale;
            } catch (RuntimeException e) {
                locale = null;
            }
        }
        return locale != null ? locale : Locale.getDefault();
    }

    public static boolean is24HourFormat(Context context) {
        return is24HourFormat(context, 0);
    }

    public static boolean is24HourFormat(Context context, int userHandle) {
        final String value = android.provider.Settings.System.getString(context != null ? context.getContentResolver() : null,
                android.provider.Settings.System.TIME_12_24);
        if (value != null) {
            return value.equals("24");
        }
        return is24HourLocale(localeOf(context));
    }

    public static boolean is24HourLocale(Locale locale) {
        synchronized (sLocaleLock) {
            if (sIs24HourLocale != null && sIs24HourLocale.equals(locale)) {
                return sIs24Hour;
            }
        }
        final java.text.DateFormat natural = java.text.DateFormat.getTimeInstance(java.text.DateFormat.LONG, locale);
        final boolean is24Hour = natural instanceof SimpleDateFormat && hasDesignator(((SimpleDateFormat) natural).toPattern(), 'H');
        synchronized (sLocaleLock) {
            sIs24HourLocale = locale;
            sIs24Hour = is24Hour;
        }
        return is24Hour;
    }

    /*
     * Patterns found lately, by skeleton and locale (as Android 9's libcore.icu.ICU keeps them): an app that asks for its
     * formats as it binds a list's rows finds them here, not by ICU again.
     */
    private static final java.util.LinkedHashMap<String, String> sPatterns = new java.util.LinkedHashMap<String, String>(16, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(java.util.Map.Entry<String, String> eldest) {
            return size() > 32;
        }
    };

    public static String getBestDateTimePattern(Locale locale, String skeleton) {
        return bestPattern(locale, skeleton, android.icu.text.DateTimePatternGenerator.MATCH_HOUR_FIELD_LENGTH);
    }

    /* ICU's best pattern for the skeleton with those options, or the skeleton where ICU cannot be had. */
    private static String bestPattern(Locale locale, String skeleton, int options) {
        final String key = skeleton + "\t" + options + "\t" + locale.toLanguageTag();
        synchronized (sPatterns) {
            final String known = sPatterns.get(key);
            if (known != null) {
                return known;
            }
        }
        String pattern;
        try {
            final android.icu.text.DateTimePatternGenerator dtpg = android.icu.text.DateTimePatternGenerator.getInstance(locale);
            pattern = dtpg.getBestPattern(skeleton, options);
        } catch (RuntimeException | LinkageError e) {
            android.util.Log.w("DateFormat", "ICU's DateTimePatternGenerator: " + e);
            return skeleton;
        }
        if (pattern == null) {
            return skeleton;
        }
        synchronized (sPatterns) {
            sPatterns.put(key, pattern);
        }
        return pattern;
    }

    public static java.text.DateFormat getTimeFormat(Context context) {
        return new SimpleDateFormat(getTimeFormatString(context), localeOf(context));
    }

    public static String getTimeFormatString(Context context) {
        return getTimeFormatString(context, 0);
    }

    /* Android 12's: the generator's best for "Hm" or "hm", the hour as long as the locale's own pattern has it. */
    public static String getTimeFormatString(Context context, int userHandle) {
        final Locale locale = localeOf(context);
        return is24HourFormat(context, userHandle) ? bestPattern(locale, "Hm", 0) : bestPattern(locale, "hm", 0);
    }

    public static java.text.DateFormat getDateFormat(Context context) {
        return java.text.DateFormat.getDateInstance(java.text.DateFormat.SHORT, localeOf(context));
    }

    public static java.text.DateFormat getLongDateFormat(Context context) {
        return java.text.DateFormat.getDateInstance(java.text.DateFormat.LONG, localeOf(context));
    }

    public static java.text.DateFormat getMediumDateFormat(Context context) {
        return java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM, localeOf(context));
    }

    public static char[] getDateFormatOrder(Context context) {
        return getDateFormatOrder(getDateFormatString(context));
    }

    private static String getDateFormatString(Context context) {
        final java.text.DateFormat df = java.text.DateFormat.getDateInstance(java.text.DateFormat.SHORT, localeOf(context));
        if (df instanceof SimpleDateFormat) {
            return ((SimpleDateFormat) df).toPattern();
        }
        throw new AssertionError("!(df instanceof SimpleDateFormat)");
    }

    /** The order of the day, month and year in a pattern: 'd', 'M' and 'y', each once at most (libcore.icu.ICU's). */
    public static char[] getDateFormatOrder(String pattern) {
        final char[] result = new char[3];
        int resultIndex = 0;
        boolean sawDay = false, sawMonth = false, sawYear = false;
        for (int i = 0; i < pattern.length(); ++i) {
            final char ch = pattern.charAt(i);
            if (ch == 'd' || ch == 'L' || ch == 'M' || ch == 'y') {
                if (ch == 'd' && !sawDay) {
                    result[resultIndex++] = 'd';
                    sawDay = true;
                } else if ((ch == 'L' || ch == 'M') && !sawMonth) {
                    result[resultIndex++] = 'M';
                    sawMonth = true;
                } else if (ch == 'y' && !sawYear) {
                    result[resultIndex++] = 'y';
                    sawYear = true;
                }
            } else if (ch == 'G') {
                // The era, where there is one, is not one of the three.
            } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
                throw new IllegalArgumentException("Bad pattern character '" + ch + "' in " + pattern);
            } else if (ch == QUOTE) {
                if (i < pattern.length() - 1 && pattern.charAt(i + 1) == QUOTE) {
                    ++i;
                } else {
                    i = pattern.indexOf(QUOTE, i + 1);
                    if (i == -1) {
                        throw new IllegalArgumentException("Bad quoting in " + pattern);
                    }
                    ++i;
                }
            }
        }
        return result;
    }

    public static CharSequence format(CharSequence inFormat, long inTimeInMillis) {
        return format(inFormat, new Date(inTimeInMillis));
    }

    public static CharSequence format(CharSequence inFormat, Date inDate) {
        final Calendar c = new GregorianCalendar();
        c.setTime(inDate);
        return format(inFormat, c);
    }

    public static boolean hasSeconds(CharSequence inFormat) {
        return hasDesignator(inFormat, SECONDS);
    }

    public static boolean hasDesignator(CharSequence inFormat, char designator) {
        if (inFormat == null) return false;
        final int length = inFormat.length();
        boolean insideQuote = false;
        for (int i = 0; i < length; i++) {
            final char c = inFormat.charAt(i);
            if (c == QUOTE) {
                insideQuote = !insideQuote;
            } else if (!insideQuote && c == designator) {
                return true;
            }
        }
        return false;
    }

    public static CharSequence format(CharSequence inFormat, Calendar inDate) {
        final SpannableStringBuilder s = new SpannableStringBuilder(inFormat);
        final android.icu.text.DateFormatSymbols dfs = getIcuDateFormatSymbols(Locale.getDefault());
        final String[] amPm = dfs.getAmPmStrings();
        int count;
        int len = inFormat.length();
        for (int i = 0; i < len; i += count) {
            count = 1;
            final int c = s.charAt(i);
            if (c == QUOTE) {
                count = appendQuotedText(s, i);
                len = s.length();
                continue;
            }
            while ((i + count < len) && (s.charAt(i + count) == c)) {
                count++;
            }
            String replacement;
            switch (c) {
                case 'A':
                case 'a':
                    replacement = amPm[inDate.get(Calendar.AM_PM) - Calendar.AM];
                    break;
                case 'd':
                    replacement = zeroPad(inDate.get(Calendar.DATE), count);
                    break;
                case 'c':
                case 'E':
                    replacement = getDayOfWeekString(dfs, inDate.get(Calendar.DAY_OF_WEEK), count, c);
                    break;
                case 'K': // the hour in the half day, 0 to 11
                case 'h': // the hour in the half day, 1 to 12
                {
                    int hour = inDate.get(Calendar.HOUR);
                    if (c == 'h' && hour == 0) {
                        hour = 12;
                    }
                    replacement = zeroPad(hour, count);
                    break;
                }
                case 'H': // the hour of the day, 0 to 23
                case 'k': // the same: Android has always read 'k' as 'H', and the apps that want 24 hours write 'k'
                    replacement = zeroPad(inDate.get(Calendar.HOUR_OF_DAY), count);
                    break;
                case 'L':
                case 'M':
                    replacement = getMonthString(dfs, inDate.get(Calendar.MONTH), count, c);
                    break;
                case 'm':
                    replacement = zeroPad(inDate.get(Calendar.MINUTE), count);
                    break;
                case 's':
                    replacement = zeroPad(inDate.get(Calendar.SECOND), count);
                    break;
                case 'y':
                    replacement = getYearString(inDate.get(Calendar.YEAR), count);
                    break;
                case 'z':
                    replacement = getTimeZoneString(inDate, count);
                    break;
                default:
                    replacement = null;
                    break;
            }
            if (replacement != null) {
                s.replace(i, i + count, replacement);
                count = replacement.length(); // the loop moves on past what was put in
                len = s.length();
            }
        }
        if (inFormat instanceof Spanned) {
            return new SpannedString(s);
        }
        return s.toString();
    }

    private static String getDayOfWeekString(android.icu.text.DateFormatSymbols dfs, int day, int count, int kind) {
        final boolean standalone = (kind == 'c');
        final int context = standalone ? android.icu.text.DateFormatSymbols.STANDALONE : android.icu.text.DateFormatSymbols.FORMAT;
        final int width;
        if (count == 5) {
            width = android.icu.text.DateFormatSymbols.NARROW;
        } else if (count == 4) {
            width = android.icu.text.DateFormatSymbols.WIDE;
        } else {
            width = android.icu.text.DateFormatSymbols.ABBREVIATED;
        }
        return dfs.getWeekdays(context, width)[day];
    }

    private static String getMonthString(android.icu.text.DateFormatSymbols dfs, int month, int count, int kind) {
        final boolean standalone = (kind == 'L');
        final int monthContext = standalone ? android.icu.text.DateFormatSymbols.STANDALONE : android.icu.text.DateFormatSymbols.FORMAT;
        if (count == 5) {
            return dfs.getMonths(monthContext, android.icu.text.DateFormatSymbols.NARROW)[month];
        } else if (count == 4) {
            return dfs.getMonths(monthContext, android.icu.text.DateFormatSymbols.WIDE)[month];
        } else if (count == 3) {
            return dfs.getMonths(monthContext, android.icu.text.DateFormatSymbols.ABBREVIATED)[month];
        } else {
            // January is 0.
            return zeroPad(month + 1, count);
        }
    }

    private static String getTimeZoneString(Calendar inDate, int count) {
        final TimeZone tz = inDate.getTimeZone();
        if (count < 2) { // (Android's: one letter is the offset, two or more the zone's short name)
            return formatZoneOffset(inDate.get(Calendar.DST_OFFSET) + inDate.get(Calendar.ZONE_OFFSET), count);
        } else {
            final boolean dst = inDate.get(Calendar.DST_OFFSET) != 0;
            return tz.getDisplayName(dst, TimeZone.SHORT);
        }
    }

    private static String formatZoneOffset(int offset, int count) {
        offset /= 1000; // in seconds
        final StringBuilder tb = new StringBuilder();
        if (offset < 0) {
            tb.insert(0, "-");
            offset = -offset;
        } else {
            tb.insert(0, "+");
        }
        final int hours = offset / 3600;
        final int minutes = (offset - (hours * 3600)) / 60;
        tb.append(zeroPad(hours, 2));
        tb.append(zeroPad(minutes, 2));
        return tb.toString();
    }

    private static String getYearString(int year, int count) {
        return (count <= 2) ? zeroPad(year % 100, 2) : String.format(Locale.getDefault(), "%d", year);
    }

    /**
     * Takes the quotes out of the quoted text that starts at `index` of the pattern, two quotes in a row being one: the
     * length of the text that stays.
     */
    public static int appendQuotedText(SpannableStringBuilder formatString, int index) {
        int length = formatString.length();
        if (index + 1 < length && formatString.charAt(index + 1) == QUOTE) {
            formatString.delete(index, index + 1);
            return 1;
        }
        int count = 0;
        // the opening quote
        formatString.delete(index, index + 1);
        length--;
        while (index < length) {
            final char c = formatString.charAt(index);
            if (c == QUOTE) {
                if (index + 1 < length && formatString.charAt(index + 1) == QUOTE) {
                    // two quotes, one kept
                    formatString.delete(index, index + 1);
                    length--;
                    count++;
                    index++;
                } else {
                    // the closing quote
                    formatString.delete(index, index + 1);
                    break;
                }
            } else {
                index++;
                count++;
            }
        }
        return count;
    }

    private static String zeroPad(int inValue, int inMinDigits) {
        return String.format(Locale.getDefault(), "%0" + inMinDigits + "d", inValue);
    }

    /** ICU's names for the Gregorian calendar in a locale: its standalone and narrow forms besides those of java.text. */
    static android.icu.text.DateFormatSymbols getIcuDateFormatSymbols(Locale locale) {
        return new android.icu.text.DateFormatSymbols(android.icu.util.GregorianCalendar.class, locale);
    }
}
