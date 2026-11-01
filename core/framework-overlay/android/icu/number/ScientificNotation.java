/* Megler: a number with an exponent (NumberFormatter.java says why this package is here). */
package android.icu.number;

public class ScientificNotation extends Notation {
    final int engineeringInterval;     /* 1 scientific; 3 engineering: the exponent a multiple of it */
    final int minExponentDigits;
    final NumberFormatter.SignDisplay exponentSign;

    ScientificNotation(int engineeringInterval, int minExponentDigits, NumberFormatter.SignDisplay exponentSign) {
        this.engineeringInterval = engineeringInterval;
        this.minExponentDigits = minExponentDigits;
        this.exponentSign = exponentSign;
    }

    public ScientificNotation withMinExponentDigits(int minExponentDigits) {
        if (minExponentDigits < 1 || minExponentDigits > 999) throw new IllegalArgumentException("Integer digits must be between 1 and 999 (inclusive)");
        return new ScientificNotation(engineeringInterval, minExponentDigits, exponentSign);
    }

    public ScientificNotation withExponentSignDisplay(NumberFormatter.SignDisplay exponentSignDisplay) {
        return new ScientificNotation(engineeringInterval, minExponentDigits, exponentSignDisplay);
    }

    @Override public boolean equals(Object o) {
        if (!(o instanceof ScientificNotation)) return false;
        ScientificNotation n = (ScientificNotation) o;
        return engineeringInterval == n.engineeringInterval && minExponentDigits == n.minExponentDigits && exponentSign == n.exponentSign;
    }

    @Override public int hashCode() { return engineeringInterval * 31 * 31 + minExponentDigits * 31 + (exponentSign == null ? 0 : exponentSign.hashCode()); }
}
