/* Megler: rounding to fraction digits, which may also count significant ones (NumberFormatter.java says why this package is here). */
package android.icu.number;

public class FractionPrecision extends Precision {
    FractionPrecision(int minFraction, int maxFraction) { super(FRACTION, minFraction, maxFraction, -1, -1, null, null, null, null, null); }

    /* Rounded to the fraction digits, but keeping at least this many significant digits. */
    public Precision withMinDigits(int minSignificantDigits) {
        checkSignificant(minSignificantDigits);
        return fractionSignificant(minFraction, maxFraction, minSignificantDigits, -1, null);
    }

    /* Rounded to the fraction digits, or to this many significant digits where that keeps fewer. */
    public Precision withMaxDigits(int maxSignificantDigits) {
        checkSignificant(maxSignificantDigits);
        return fractionSignificant(minFraction, maxFraction, -1, maxSignificantDigits, null);
    }

    /* Both: RELAXED keeps the more digits of the two roundings, STRICT the fewer. */
    public Precision withSignificantDigits(int minSignificantDigits, int maxSignificantDigits, NumberFormatter.RoundingPriority priority) {
        checkSignificant(minSignificantDigits);
        checkSignificant(maxSignificantDigits);
        if (minSignificantDigits > maxSignificantDigits) throw new IllegalArgumentException("The minimum of significant digits is more than the maximum");
        return fractionSignificant(minFraction, maxFraction, minSignificantDigits, maxSignificantDigits, priority == null ? NumberFormatter.RoundingPriority.RELAXED : priority);
    }
}
