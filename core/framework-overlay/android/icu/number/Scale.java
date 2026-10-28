/* Megler: a factor a number is multiplied by before it is shown (NumberFormatter.java says why this package is here). */
package android.icu.number;

import java.math.BigDecimal;

public class Scale {
    private static final Scale NONE = new Scale(null);

    final BigDecimal factor;           /* null: none */

    private Scale(BigDecimal factor) { this.factor = factor; }

    public static Scale none() { return NONE; }

    public static Scale powerOfTen(int power) { return power == 0 ? NONE : new Scale(BigDecimal.ONE.scaleByPowerOfTen(power)); }

    public static Scale byBigDecimal(BigDecimal multiplicand) {
        return multiplicand == null || multiplicand.compareTo(BigDecimal.ONE) == 0 ? NONE : new Scale(multiplicand);
    }

    public static Scale byDouble(double multiplicand) { return multiplicand == 1 ? NONE : new Scale(BigDecimal.valueOf(multiplicand)); }

    public static Scale byDoubleAndPowerOfTen(double multiplicand, int power) { return new Scale(BigDecimal.valueOf(multiplicand).scaleByPowerOfTen(power)); }

    BigDecimal applyTo(BigDecimal value) { return factor == null ? value : value.multiply(factor); }

    @Override public boolean equals(Object o) {
        return o instanceof Scale && (factor == null ? ((Scale) o).factor == null : ((Scale) o).factor != null && factor.compareTo(((Scale) o).factor) == 0);
    }

    @Override public int hashCode() { return factor == null ? 0 : factor.stripTrailingZeros().hashCode(); }
}
