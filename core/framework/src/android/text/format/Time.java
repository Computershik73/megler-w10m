package android.text.format;

import android.util.TimeFormatException;
import java.util.Calendar;
import java.util.GregorianCalendar;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Android's Time on java.util's calendar: the fields are a wall time in {@link #timezone}, and every method behaves as
 * Android's does. normalize, set(day, month, year), the Julian days, compare, parse and parse3339, format2445 and
 * format3339 were their signatures alone before (normalize gave 0 and changed nothing), and an app that keeps its dates in
 * Times (ColorNote's calendar, Accuweather's database) found every date the epoch. format is strftime's, as Android's.
 */
public class Time {
    public static final String TIMEZONE_UTC = "UTC";
    public static final int EPOCH_JULIAN_DAY = 2440588;
    public static final int MONDAY_BEFORE_JULIAN_EPOCH = 2440585;
    public static final int SECOND = 1;
    public static final int MINUTE = 2;
    public static final int HOUR = 3;
    public static final int MONTH_DAY = 4;
    public static final int MONTH = 5;
    public static final int YEAR = 6;
    public static final int WEEK_DAY = 7;
    public static final int YEAR_DAY = 8;
    public static final int WEEK_NUM = 9;
    public static final int SUNDAY = 0;
    public static final int MONDAY = 1;
    public static final int TUESDAY = 2;
    public static final int WEDNESDAY = 3;
    public static final int THURSDAY = 4;
    public static final int FRIDAY = 5;
    public static final int SATURDAY = 6;

    private static final long DAY_MILLIS = 24L * 60 * 60 * 1000;
    private static final int[] DAYS_PER_MONTH = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    // (From a week day to that week's Thursday, for the ISO week number.)
    private static final int[] THURSDAY_OFFSET = { -3, 3, 2, 1, 0, -1, -2 };

    public boolean allDay;
    public int second, minute, hour, monthDay, month, year, weekDay, yearDay, isDst;
    public long gmtoff;
    public String timezone;

    public Time(String timezoneId) {
        if (timezoneId == null) throw new NullPointerException("timezoneId is null!");
        clearTo(timezoneId);
        year = 1970;
        monthDay = 1;
    }
    public Time() { this(TimeZone.getDefault().getID()); }
    public Time(Time other) { this(other.timezone); set(other); }

    private void clearTo(String timezoneId) {
        timezone = timezoneId;
        allDay = false;
        second = minute = hour = monthDay = month = year = weekDay = yearDay = 0;
        gmtoff = 0;
        isDst = -1;
    }

    private TimeZone zone() { return TimeZone.getTimeZone(timezone); }

    /** The fields from a moment in this time's zone, every one of them right afterwards. allDay is left as it is. */
    private void fill(long millis) {
        TimeZone tz = zone();
        Calendar c = new GregorianCalendar(tz, Locale.US);
        c.setTimeInMillis(millis);
        second = c.get(Calendar.SECOND);
        minute = c.get(Calendar.MINUTE);
        hour = c.get(Calendar.HOUR_OF_DAY);
        monthDay = c.get(Calendar.DAY_OF_MONTH);
        month = c.get(Calendar.MONTH);
        year = c.get(Calendar.ERA) == GregorianCalendar.BC ? 1 - c.get(Calendar.YEAR) : c.get(Calendar.YEAR);
        weekDay = c.get(Calendar.DAY_OF_WEEK) - 1;
        yearDay = c.get(Calendar.DAY_OF_YEAR) - 1;
        isDst = tz.inDaylightTime(new java.util.Date(millis)) ? 1 : 0;
        gmtoff = tz.getOffset(millis) / 1000;
    }

    public void clear(String timezoneId) {
        if (timezoneId == null) throw new NullPointerException("timezone is null!");
        clearTo(timezoneId);
    }

    public long toMillis(boolean ignoreDst) {
        TimeZone tz = zone();
        Calendar c = new GregorianCalendar(tz, Locale.US);
        c.setLenient(true);
        c.clear();
        c.set(year, month, monthDay, hour, minute, second);
        long millis = c.getTimeInMillis();
        // (A known isDst that is not the zone's at that time moves the moment by the saving, as Android's does: the wall
        // time read with the offset isDst says.)
        if (!ignoreDst && isDst >= 0) {
            int actual = tz.inDaylightTime(new java.util.Date(millis)) ? 1 : 0;
            if (actual != isDst) millis += (long) (actual - isDst) * tz.getDSTSavings();
        }
        return millis;
    }

    public long normalize(boolean ignoreDst) {
        long millis = toMillis(ignoreDst);
        fill(millis);
        return millis;
    }

    public void switchTimezone(String timezone) {
        long millis = toMillis(true);
        this.timezone = timezone;
        fill(millis);
    }

    public void set(long millis) {
        allDay = false;
        fill(millis);
    }
    public void setToNow() { set(System.currentTimeMillis()); }

    public void set(int second, int minute, int hour, int monthDay, int month, int year) {
        allDay = false;
        this.second = second;
        this.minute = minute;
        this.hour = hour;
        this.monthDay = monthDay;
        this.month = month;
        this.year = year;
        weekDay = 0;
        yearDay = 0;
        isDst = -1;
        gmtoff = 0;
    }

    public void set(int monthDay, int month, int year) {
        set(0, 0, 0, monthDay, month, year);
        allDay = true;
    }

    public void set(Time that) {
        timezone = that.timezone;
        allDay = that.allDay;
        second = that.second;
        minute = that.minute;
        hour = that.hour;
        monthDay = that.monthDay;
        month = that.month;
        year = that.year;
        weekDay = that.weekDay;
        yearDay = that.yearDay;
        isDst = that.isDst;
        gmtoff = that.gmtoff;
    }

    public static int compare(Time a, Time b) {
        if (a == null) throw new NullPointerException("a == null");
        if (b == null) throw new NullPointerException("b == null");
        if (a.timezone.equals(b.timezone)) {
            int d = a.year - b.year;
            if (d == 0) d = a.month - b.month;
            if (d == 0) d = a.monthDay - b.monthDay;
            if (d == 0) d = a.hour - b.hour;
            if (d == 0) d = a.minute - b.minute;
            if (d == 0) d = a.second - b.second;
            return Integer.signum(d);
        }
        return Long.signum(a.toMillis(false) - b.toMillis(false));
    }
    public boolean after(Time that) { return compare(this, that) > 0; }
    public boolean before(Time that) { return compare(this, that) < 0; }

    public static String getCurrentTimezone() { return TimeZone.getDefault().getID(); }

    public static int getJulianDay(long millis, long gmtoff) {
        return (int) ((millis + gmtoff * 1000) / DAY_MILLIS) + EPOCH_JULIAN_DAY;
    }

    public long setJulianDay(int julianDay) {
        // (Near the day by its midnight in UTC, then onto it by the day's difference in this zone, at its midnight.)
        long millis = (julianDay - EPOCH_JULIAN_DAY) * DAY_MILLIS;
        set(millis);
        monthDay += julianDay - getJulianDay(millis, gmtoff);
        hour = 0;
        minute = 0;
        second = 0;
        return normalize(true);
    }

    public static int getWeeksSinceEpochFromJulianDay(int julianDay, int firstDayOfWeek) {
        int diff = THURSDAY - firstDayOfWeek;
        if (diff < 0) diff += 7;
        return (julianDay - (EPOCH_JULIAN_DAY - diff)) / 7;
    }

    public static int getJulianMondayFromWeeksSinceEpoch(int week) { return MONDAY_BEFORE_JULIAN_EPOCH + week * 7; }

    public static boolean isEpoch(Time time) { return getJulianDay(time.toMillis(true), 0) == EPOCH_JULIAN_DAY; }

    public int getWeekNumber() {
        int thursday = yearDay + THURSDAY_OFFSET[weekDay];
        if (thursday >= 0 && thursday <= 364) return thursday / 7 + 1;
        // (The week crosses into the next or the last year: its Thursday's year counts.)
        Time t = new Time(this);
        t.monthDay += THURSDAY_OFFSET[weekDay];
        t.normalize(true);
        return t.yearDay / 7 + 1;
    }

    private static boolean leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

    public int getActualMaximum(int field) {
        switch (field) {
            case SECOND: return 59;
            case MINUTE: return 59;
            case HOUR: return 23;
            case MONTH_DAY: return month == 1 ? (leap(year) ? 29 : 28) : DAYS_PER_MONTH[month];
            case MONTH: return 11;
            case YEAR: return 2037;
            case WEEK_DAY: return 6;
            case YEAR_DAY: return leap(year) ? 365 : 364;
            case WEEK_NUM: throw new RuntimeException("WEEK_NUM not implemented");
            default: throw new RuntimeException("bad field=" + field);
        }
    }

    private static int digits(String s, int at, int n) {
        int v = 0;
        for (int i = at; i < at + n; i++) {
            char c = s.charAt(i);
            if (!Character.isDigit(c)) throw new TimeFormatException("Parse error at pos=" + i);
            v = v * 10 + Character.digit(c, 10);
        }
        return v;
    }
    private static void expect(String s, int at, char c) {
        if (s.charAt(at) != c) {
            throw new TimeFormatException(String.format(Locale.US, "Unexpected character 0x%02d at pos=%d.  Expected 0x%02d ('%c').",
                (int) s.charAt(at), at, (int) c, c));
        }
    }

    /** RFC 2445's YYYYMMDD, YYYYMMDDTHHMMSS or YYYYMMDDTHHMMSSZ; true (and the zone UTC) for the last. */
    public boolean parse(String s) {
        if (s == null) throw new NullPointerException("time string is null");
        int len = s.length();
        if (len < 8) throw new TimeFormatException("String is too short: \"" + s + "\" Expected at least 8 characters.");
        boolean utc = false;
        year = digits(s, 0, 4);
        month = digits(s, 4, 2) - 1;
        monthDay = digits(s, 6, 2);
        if (len > 8) {
            if (len < 15) throw new TimeFormatException("String is too short: \"" + s + "\" If there are more than 8 characters there must be at least 15.");
            expect(s, 8, 'T');
            allDay = false;
            hour = digits(s, 9, 2);
            minute = digits(s, 11, 2);
            second = digits(s, 13, 2);
            if (len > 15) {
                expect(s, 15, 'Z');
                utc = true;
            }
        } else {
            allDay = true;
            hour = minute = second = 0;
        }
        weekDay = 0;
        yearDay = 0;
        isDst = -1;
        gmtoff = 0;
        if (utc) timezone = TIMEZONE_UTC;
        return utc;
    }

    /** RFC 3339's YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS[.fraction][Z|±HH:MM]; true (and the time in UTC) with a zone. */
    public boolean parse3339(String s) {
        if (s == null) throw new NullPointerException("time string is null");
        int len = s.length();
        if (len < 10) throw new TimeFormatException("String too short --- expected at least 10 characters.");
        boolean utc = false;
        year = digits(s, 0, 4);
        expect(s, 4, '-');
        month = digits(s, 5, 2) - 1;
        expect(s, 7, '-');
        monthDay = digits(s, 8, 2);
        if (len >= 19) {
            expect(s, 10, 'T');
            allDay = false;
            int h = digits(s, 11, 2);
            expect(s, 13, ':');
            int m = digits(s, 14, 2);
            expect(s, 16, ':');
            second = digits(s, 17, 2);
            int at = 19;
            if (at < len && s.charAt(at) == '.') {
                do at++; while (at < len && Character.isDigit(s.charAt(at)));
            }
            int sign = 0;
            if (len > at) {
                char c = s.charAt(at);
                if (c == 'Z') sign = 0;
                else if (c == '-') sign = 1;
                else if (c == '+') sign = -1;
                else throw new TimeFormatException(String.format(Locale.US, "Unexpected character 0x%02d at position %d.  Expected + or -", (int) c, at));
                utc = true;
                if (sign != 0) {
                    if (len < at + 6) throw new TimeFormatException(String.format(Locale.US, "Unexpected length; should be %d characters", at + 6));
                    // (The offset taken off, which brings the wall time to UTC's.)
                    h += sign * digits(s, at + 1, 2);
                    m += sign * digits(s, at + 4, 2);
                }
            }
            hour = h;
            minute = m;
            if (utc) timezone = TIMEZONE_UTC;
            if (sign != 0) normalize(false);
        } else {
            allDay = true;
            hour = minute = second = 0;
        }
        weekDay = 0;
        yearDay = 0;
        isDst = -1;
        gmtoff = 0;
        return utc;
    }

    public String format2445() {
        StringBuilder b = new StringBuilder(16);
        pad(b, year, 4, '0');
        pad(b, month + 1, 2, '0');
        pad(b, monthDay, 2, '0');
        if (allDay) return b.toString();
        b.append('T');
        pad(b, hour, 2, '0');
        pad(b, minute, 2, '0');
        pad(b, second, 2, '0');
        if (TIMEZONE_UTC.equals(timezone)) b.append('Z');
        return b.toString();
    }

    public String format3339(boolean allDay) {
        if (allDay) return format("%Y-%m-%d");
        if (TIMEZONE_UTC.equals(timezone)) return format("%Y-%m-%dT%H:%M:%S.000Z");
        int offset = (int) Math.abs(gmtoff);
        return String.format(Locale.US, "%s%s%02d:%02d", format("%Y-%m-%dT%H:%M:%S.000"), gmtoff < 0 ? "-" : "+", offset / 3600, (offset % 3600) / 60);
    }

    private static void pad(StringBuilder b, int v, int width, char with) {
        String s = Integer.toString(Math.abs(v));
        if (v < 0) b.append('-');
        for (int i = s.length(); i < width; i++) b.append(with);
        b.append(s);
    }

    /** strftime's conversions, with the default locale's names, and its flags - (no padding), _ (spaces), 0 and ^ (capitals). */
    public String format(String f) {
        java.text.DateFormatSymbols names = java.text.DateFormatSymbols.getInstance(Locale.getDefault());
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < f.length(); i++) {
            char c = f.charAt(i);
            if (c != '%' || i + 1 >= f.length()) { b.append(c); continue; }
            char pad = 0;
            boolean upper = false;
            c = f.charAt(++i);
            while ((c == '-' || c == '_' || c == '0' || c == '^' || c == '#') && i + 1 < f.length()) {
                if (c == '^') upper = true;
                else if (c != '#') pad = c;
                c = f.charAt(++i);
            }
            int start = b.length();
            conversion(b, c, pad, names);
            if (upper) b.replace(start, b.length(), b.substring(start).toUpperCase(Locale.getDefault()));
        }
        return b.toString();
    }

    private void number(StringBuilder b, int v, int width, char defaultPad, char pad) {
        if (pad == '-') b.append(v);
        else pad(b, v, width, pad == '_' ? ' ' : pad == '0' ? '0' : defaultPad);
    }

    private void conversion(StringBuilder b, char c, char pad, java.text.DateFormatSymbols names) {
        int h12 = hour % 12 == 0 ? 12 : hour % 12;
        switch (c) {
            case 'a': b.append(names.getShortWeekdays()[weekDay + 1]); break;
            case 'A': b.append(names.getWeekdays()[weekDay + 1]); break;
            case 'b': case 'h': b.append(names.getShortMonths()[month]); break;
            case 'B': b.append(names.getMonths()[month]); break;
            case 'c': b.append(format("%a %b %e %H:%M:%S %Y")); break;
            case 'C': number(b, year / 100, 2, '0', pad); break;
            case 'd': number(b, monthDay, 2, '0', pad); break;
            case 'D': b.append(format("%m/%d/%y")); break;
            case 'e': number(b, monthDay, 2, ' ', pad); break;
            case 'F': b.append(format("%Y-%m-%d")); break;
            case 'g': case 'G': {
                // (The ISO week's year: the year of its Thursday.)
                Time t = new Time(this);
                t.monthDay += THURSDAY_OFFSET[weekDay];
                t.normalize(true);
                if (c == 'G') number(b, t.year, 4, '0', pad); else number(b, Math.abs(t.year) % 100, 2, '0', pad);
                break;
            }
            case 'H': number(b, hour, 2, '0', pad); break;
            case 'I': number(b, h12, 2, '0', pad); break;
            case 'j': number(b, yearDay + 1, 3, '0', pad); break;
            case 'k': number(b, hour, 2, ' ', pad); break;
            case 'l': number(b, h12, 2, ' ', pad); break;
            case 'm': number(b, month + 1, 2, '0', pad); break;
            case 'M': number(b, minute, 2, '0', pad); break;
            case 'n': b.append('\n'); break;
            case 'p': b.append(names.getAmPmStrings()[hour < 12 ? 0 : 1]); break;
            case 'P': b.append(names.getAmPmStrings()[hour < 12 ? 0 : 1].toLowerCase(Locale.getDefault())); break;
            case 'r': b.append(format("%I:%M:%S %p")); break;
            case 'R': b.append(format("%H:%M")); break;
            case 's': b.append(toMillis(false) / 1000); break;
            case 'S': number(b, second, 2, '0', pad); break;
            case 't': b.append('\t'); break;
            case 'T': b.append(format("%H:%M:%S")); break;
            case 'u': number(b, weekDay == 0 ? 7 : weekDay, 1, '0', pad); break;
            case 'U': number(b, (yearDay + 7 - weekDay) / 7, 2, '0', pad); break;
            case 'V': number(b, getWeekNumber(), 2, '0', pad); break;
            case 'w': number(b, weekDay, 1, '0', pad); break;
            case 'W': number(b, (yearDay + 7 - (weekDay == 0 ? 6 : weekDay - 1)) / 7, 2, '0', pad); break;
            case 'x': b.append(format("%m/%d/%y")); break;
            case 'X': b.append(format("%H:%M:%S")); break;
            case 'y': number(b, Math.abs(year) % 100, 2, '0', pad); break;
            case 'Y': number(b, year, 4, '0', pad); break;
            case 'z': {
                long off = Math.abs(gmtoff) / 60;
                b.append(gmtoff < 0 ? '-' : '+');
                pad(b, (int) (off / 60), 2, '0');
                pad(b, (int) (off % 60), 2, '0');
                break;
            }
            case 'Z': b.append(zone().getDisplayName(isDst > 0, TimeZone.SHORT, Locale.getDefault())); break;
            case '+': b.append(format("%a %b %e %H:%M:%S %Z %Y")); break;
            case '%': b.append('%'); break;
            default: b.append('%').append(c); break;
        }
    }

    @Override
    public String toString() {
        return String.format(Locale.US, "%04d%02d%02dT%02d%02d%02d%s(%d,%d,%d,%d,%d)", year, month + 1, monthDay, hour, minute, second,
            timezone, weekDay, yearDay, gmtoff, isDst, toMillis(false) / 1000);
    }
}
