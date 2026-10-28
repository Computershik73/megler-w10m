/* Megler: a formatter with its locale, which formats (Formatting.java; NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.Measure;
import java.text.AttributedCharacterIterator;
import java.text.FieldPosition;
import java.text.Format;
import java.text.ParsePosition;

public class LocalizedNumberFormatter extends NumberFormatterSettings<LocalizedNumberFormatter> {
    LocalizedNumberFormatter(Macros macros) { super(macros); }

    public FormattedNumber format(long input) { return Formatting.format(macros, Long.valueOf(input)); }

    public FormattedNumber format(double input) { return Formatting.format(macros, Double.valueOf(input)); }

    public FormattedNumber format(Number input) {
        if (input == null) throw new IllegalArgumentException("Cannot format null input");
        return Formatting.format(macros, input);
    }

    /* A measure's own unit, in place of the formatter's. */
    public FormattedNumber format(Measure input) {
        if (input == null) throw new IllegalArgumentException("Cannot format null input");
        Macros m = macros.copy();
        m.unit = input.getUnit();
        return Formatting.format(m, input.getNumber());
    }

    public UnlocalizedNumberFormatter withoutLocale() {
        Macros m = macros.copy();
        m.locale = null;
        return new UnlocalizedNumberFormatter(m);
    }

    /* As a java.text.Format, which formats numbers and does not parse. */
    public Format toFormat() { return new AsFormat(this); }

    @Override LocalizedNumberFormatter create(Macros macros) { return new LocalizedNumberFormatter(macros); }

    private static final class AsFormat extends Format {
        private final LocalizedNumberFormatter formatter;

        AsFormat(LocalizedNumberFormatter formatter) { this.formatter = formatter; }

        @Override public StringBuffer format(Object obj, StringBuffer toAppendTo, FieldPosition pos) {
            if (!(obj instanceof Number)) throw new IllegalArgumentException("Cannot format " + obj);
            return toAppendTo.append(formatter.format((Number) obj));
        }

        @Override public AttributedCharacterIterator formatToCharacterIterator(Object obj) {
            if (!(obj instanceof Number)) throw new IllegalArgumentException("Cannot format " + obj);
            return formatter.format((Number) obj).toCharacterIterator();
        }

        @Override public Object parseObject(String source, ParsePosition pos) { throw new UnsupportedOperationException(); }
    }
}
