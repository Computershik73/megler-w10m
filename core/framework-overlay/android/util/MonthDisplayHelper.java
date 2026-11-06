// Overlay of core/framework/gen-src/android/util/MonthDisplayHelper.java (tools/build-framework.sh).
// It differs in being Android's helper rather than its signatures alone: a month as a grid of six weeks of seven days from a
// week's first day, which a calendar view draws its days by. The stub gave row and column 0 for every day and no digits
// (null), and ColorNote's month view put every day in one cell.
package android.util;

import java.util.Calendar;

public class MonthDisplayHelper {
    private final int mWeekStartDay;
    private final Calendar mCalendar;
    private int mDaysInMonth;
    private int mDaysInPreviousMonth;
    private int mOffset;

    public MonthDisplayHelper(int year, int month, int weekStartDay) {
        if (weekStartDay < Calendar.SUNDAY || weekStartDay > Calendar.SATURDAY) throw new IllegalArgumentException();
        mWeekStartDay = weekStartDay;
        mCalendar = Calendar.getInstance();
        mCalendar.set(Calendar.YEAR, year);
        mCalendar.set(Calendar.MONTH, month);
        mCalendar.set(Calendar.DAY_OF_MONTH, 1);
        mCalendar.set(Calendar.HOUR_OF_DAY, 0);
        mCalendar.set(Calendar.MINUTE, 0);
        mCalendar.set(Calendar.SECOND, 0);
        mCalendar.getTimeInMillis();
        recalculate();
    }

    public MonthDisplayHelper(int year, int month) { this(year, month, Calendar.SUNDAY); }

    public int getYear() { return mCalendar.get(Calendar.YEAR); }
    public int getMonth() { return mCalendar.get(Calendar.MONTH); }
    public int getWeekStartDay() { return mWeekStartDay; }
    public int getFirstDayOfMonth() { return mCalendar.get(Calendar.DAY_OF_WEEK); }
    public int getNumberOfDaysInMonth() { return mDaysInMonth; }
    /** How many cells of the first row are the last month's days. */
    public int getOffset() { return mOffset; }

    public int[] getDigitsForRow(int row) {
        if (row < 0 || row > 5) throw new IllegalArgumentException("row " + row + " out of range (0-5)");
        int[] days = new int[7];
        for (int column = 0; column < 7; column++) days[column] = getDayAt(row, column);
        return days;
    }

    /** The day of month in a cell: the last month's in the first row's leading cells, the next month's after the last day. */
    public int getDayAt(int row, int column) {
        if (row == 0 && column < mOffset) return mDaysInPreviousMonth + column - mOffset + 1;
        int day = 7 * row + column - mOffset + 1;
        return day > mDaysInMonth ? day - mDaysInMonth : day;
    }

    public int getRowOf(int day) { return (day + mOffset - 1) / 7; }
    public int getColumnOf(int day) { return (day + mOffset - 1) % 7; }

    public void previousMonth() {
        mCalendar.add(Calendar.MONTH, -1);
        recalculate();
    }

    public void nextMonth() {
        mCalendar.add(Calendar.MONTH, 1);
        recalculate();
    }

    public boolean isWithinCurrentMonth(int row, int column) {
        if (row < 0 || column < 0 || row > 5 || column > 6) return false;
        if (row == 0 && column < mOffset) return false;
        return 7 * row + column - mOffset + 1 <= mDaysInMonth;
    }

    private void recalculate() {
        mDaysInMonth = mCalendar.getActualMaximum(Calendar.DAY_OF_MONTH);
        mCalendar.add(Calendar.MONTH, -1);
        mDaysInPreviousMonth = mCalendar.getActualMaximum(Calendar.DAY_OF_MONTH);
        mCalendar.add(Calendar.MONTH, 1);
        int offset = getFirstDayOfMonth() - mWeekStartDay;
        mOffset = offset < 0 ? offset + 7 : offset;
    }
}
