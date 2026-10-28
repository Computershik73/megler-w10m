/* Megler: how many integer digits a number shows (NumberFormatter.java says why this package is here). */
package android.icu.number;

public class IntegerWidth {
    final int minInt, maxInt;          /* maxInt -1: no limit */

    private IntegerWidth(int minInt, int maxInt) {
        this.minInt = minInt;
        this.maxInt = maxInt;
    }

    public static IntegerWidth zeroFillTo(int minInt) {
        if (minInt < 0 || minInt > 999) throw new IllegalArgumentException("Integer digits must be between 0 and 999 (inclusive)");
        return new IntegerWidth(minInt, -1);
    }

    public IntegerWidth truncateAt(int maxInt) {
        if (maxInt != -1 && (maxInt < minInt || maxInt > 999)) throw new IllegalArgumentException("Integer digits must be between -1 and 999 (inclusive)");
        return new IntegerWidth(minInt, maxInt);
    }

    @Override public boolean equals(Object o) { return o instanceof IntegerWidth && ((IntegerWidth) o).minInt == minInt && ((IntegerWidth) o).maxInt == maxInt; }

    @Override public int hashCode() { return minInt * 1000 + maxInt; }
}
