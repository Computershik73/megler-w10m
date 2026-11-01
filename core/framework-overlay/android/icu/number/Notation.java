/* Megler: how a number is written: plainly, scientifically, compactly (NumberFormatter.java says why this package is here). */
package android.icu.number;

public class Notation {
    Notation() {}

    public static ScientificNotation scientific() { return new ScientificNotation(1, 1, NumberFormatter.SignDisplay.AUTO); }

    public static ScientificNotation engineering() { return new ScientificNotation(3, 1, NumberFormatter.SignDisplay.AUTO); }

    public static CompactNotation compactShort() { return CompactNotation.SHORT; }

    public static CompactNotation compactLong() { return CompactNotation.LONG; }

    public static SimpleNotation simple() { return SimpleNotation.INSTANCE; }
}
