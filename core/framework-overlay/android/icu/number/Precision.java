/* Megler: how a formatter rounds (Formatting.java applies it; NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.Currency;
import java.math.BigDecimal;

public class Precision {
    static final int UNLIMITED = 0, FRACTION = 1, SIGNIFICANT = 2, FRACTION_SIGNIFICANT = 3, INCREMENT = 4, CURRENCY = 5;
    private static final int MAX = 999;

    final int kind;
    final int minFraction, maxFraction;      /* -1: no limit */
    final int minSignificant, maxSignificant;
    /*
     * Fraction rounding with significant digits (FRACTION_SIGNIFICANT): null for withMinDigits (at least minSignificant
     * kept) and withMaxDigits (at most maxSignificant), Android 13's priority for withSignificantDigits.
     */
    final NumberFormatter.RoundingPriority priority;
    final BigDecimal increment;
    final Currency.CurrencyUsage usage;
    final Currency currency;                 /* CurrencyPrecision.withCurrency's, or null: the unit's */
    final NumberFormatter.TrailingZeroDisplay trailingZeros;

    Precision(int kind, int minFraction, int maxFraction, int minSignificant, int maxSignificant, NumberFormatter.RoundingPriority priority,
              BigDecimal increment, Currency.CurrencyUsage usage, Currency currency, NumberFormatter.TrailingZeroDisplay trailingZeros) {
        this.kind = kind;
        this.minFraction = minFraction;
        this.maxFraction = maxFraction;
        this.minSignificant = minSignificant;
        this.maxSignificant = maxSignificant;
        this.priority = priority;
        this.increment = increment;
        this.usage = usage;
        this.currency = currency;
        this.trailingZeros = trailingZeros;
    }

    static FractionPrecision fraction(int min, int max) { return new FractionPrecision(min, max); }

    private static Precision significant(int min, int max) { return new Precision(SIGNIFICANT, -1, -1, min, max, null, null, null, null, null); }

    private static void fractionDigits(int n) {
        if (n < 0 || n > MAX) throw new IllegalArgumentException("Fraction length must be between 0 and " + MAX + " (inclusive)");
    }

    private static void significantDigits(int n) {
        if (n < 1 || n > MAX) throw new IllegalArgumentException("Significant digits must be between 1 and " + MAX + " (inclusive)");
    }

    public static Precision unlimited() { return new Precision(UNLIMITED, -1, -1, -1, -1, null, null, null, null, null); }

    public static FractionPrecision integer() { return fraction(0, 0); }

    public static FractionPrecision fixedFraction(int minMaxFractionPlaces) {
        fractionDigits(minMaxFractionPlaces);
        return fraction(minMaxFractionPlaces, minMaxFractionPlaces);
    }

    public static FractionPrecision minFraction(int minFractionPlaces) {
        fractionDigits(minFractionPlaces);
        return fraction(minFractionPlaces, -1);
    }

    public static FractionPrecision maxFraction(int maxFractionPlaces) {
        fractionDigits(maxFractionPlaces);
        return fraction(0, maxFractionPlaces);
    }

    public static FractionPrecision minMaxFraction(int minFractionPlaces, int maxFractionPlaces) {
        fractionDigits(minFractionPlaces);
        fractionDigits(maxFractionPlaces);
        if (minFractionPlaces > maxFractionPlaces) throw new IllegalArgumentException("The minimum fraction length is more than the maximum");
        return fraction(minFractionPlaces, maxFractionPlaces);
    }

    public static Precision fixedSignificantDigits(int minMaxSignificantDigits) {
        significantDigits(minMaxSignificantDigits);
        return significant(minMaxSignificantDigits, minMaxSignificantDigits);
    }

    public static Precision minSignificantDigits(int minSignificantDigits) {
        significantDigits(minSignificantDigits);
        return significant(minSignificantDigits, -1);
    }

    public static Precision maxSignificantDigits(int maxSignificantDigits) {
        significantDigits(maxSignificantDigits);
        return significant(1, maxSignificantDigits);
    }

    public static Precision minMaxSignificantDigits(int minSignificantDigits, int maxSignificantDigits) {
        significantDigits(minSignificantDigits);
        significantDigits(maxSignificantDigits);
        if (minSignificantDigits > maxSignificantDigits) throw new IllegalArgumentException("The minimum of significant digits is more than the maximum");
        return significant(minSignificantDigits, maxSignificantDigits);
    }

    public static Precision increment(BigDecimal roundingIncrement) {
        if (roundingIncrement == null || roundingIncrement.signum() <= 0) throw new IllegalArgumentException("Rounding increment must be positive and non-null");
        return new Precision(INCREMENT, -1, -1, -1, -1, null, roundingIncrement, null, null, null);
    }

    public static CurrencyPrecision currency(Currency.CurrencyUsage currencyUsage) {
        if (currencyUsage == null) throw new IllegalArgumentException("CurrencyUsage must be non-null");
        return new CurrencyPrecision(currencyUsage, null);
    }

    public Precision trailingZeroDisplay(NumberFormatter.TrailingZeroDisplay trailingZeroDisplay) {
        return new Precision(kind, minFraction, maxFraction, minSignificant, maxSignificant, priority, increment, usage, currency, trailingZeroDisplay);
    }

    @Override public boolean equals(Object o) {
        if (!(o instanceof Precision)) return false;
        Precision p = (Precision) o;
        return kind == p.kind && minFraction == p.minFraction && maxFraction == p.maxFraction && minSignificant == p.minSignificant
            && maxSignificant == p.maxSignificant && priority == p.priority && java.util.Objects.equals(increment, p.increment) && usage == p.usage
            && java.util.Objects.equals(currency, p.currency) && trailingZeros == p.trailingZeros;
    }

    @Override public int hashCode() { return java.util.Objects.hash(kind, minFraction, maxFraction, minSignificant, maxSignificant, priority, increment, usage, trailingZeros); }

    /* Fraction rounding that also counts significant digits (FractionPrecision's). */
    static Precision fractionSignificant(int minFraction, int maxFraction, int minSignificant, int maxSignificant, NumberFormatter.RoundingPriority priority) {
        return new Precision(FRACTION_SIGNIFICANT, minFraction, maxFraction, minSignificant, maxSignificant, priority, null, null, null, null);
    }

    static void checkSignificant(int n) { significantDigits(n); }
}
