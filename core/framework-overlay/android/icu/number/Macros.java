/* Megler: what a formatter has been told, each setting null until it is (NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;
import java.math.RoundingMode;
import java.util.Arrays;

final class Macros implements Cloneable {
    Notation notation;
    MeasureUnit unit, perUnit;
    Precision precision;
    RoundingMode roundingMode;
    NumberFormatter.GroupingStrategy grouping;
    IntegerWidth integerWidth;
    Object symbols;                    /* a DecimalFormatSymbols of its own, or a NumberingSystem */
    NumberFormatter.UnitWidth unitWidth;
    NumberFormatter.SignDisplay sign;
    NumberFormatter.DecimalSeparatorDisplay decimal;
    Scale scale;
    String usage;
    ULocale locale;

    Macros copy() {
        try {
            return (Macros) clone();
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }

    private Object[] all() {
        return new Object[] { notation, unit, perUnit, precision, roundingMode, grouping, integerWidth, symbols, unitWidth, sign, decimal, scale, usage, locale };
    }

    @Override public boolean equals(Object o) { return o instanceof Macros && Arrays.equals(all(), ((Macros) o).all()); }

    @Override public int hashCode() { return Arrays.hashCode(all()); }
}
