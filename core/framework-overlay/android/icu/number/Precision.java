package android.icu.number;

/** Fraction rounding used by the Android 30 NumberFormatter API. */
public class Precision {
    final int minFraction;
    final int maxFraction;

    Precision(int minFraction, int maxFraction) {
        this.minFraction = minFraction;
        this.maxFraction = maxFraction;
    }

    public static FractionPrecision fixedFraction(int places) {
        if (places < 0) throw new IllegalArgumentException("places < 0");
        return new FractionPrecision(places, places);
    }

    public static FractionPrecision maxFraction(int places) {
        if (places < 0) throw new IllegalArgumentException("places < 0");
        return new FractionPrecision(0, places);
    }
}
