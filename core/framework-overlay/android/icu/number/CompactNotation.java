/* Megler: a number in thousands, millions (NumberFormatter.java says why this package is here). */
package android.icu.number;

public class CompactNotation extends Notation {
    static final CompactNotation SHORT = new CompactNotation(false), LONG = new CompactNotation(true);

    final boolean longNames;           /* "1.2 thousand" rather than "1.2K" */

    private CompactNotation(boolean longNames) { this.longNames = longNames; }
}
