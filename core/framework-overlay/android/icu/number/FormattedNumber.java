/* Megler: a formatted number (NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.MeasureUnit;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.math.BigDecimal;
import java.text.AttributedCharacterIterator;
import java.text.AttributedString;
import java.text.Format;

public class FormattedNumber implements CharSequence {
    private final String text;
    private final Format format;       /* what made the text alone, for its fields; null when more went into it */
    private final Object formatted;
    private final BigDecimal value;
    private final MeasureUnit outputUnit;

    FormattedNumber(String text, Format format, Object formatted, BigDecimal value, MeasureUnit outputUnit) {
        this.text = text;
        this.format = format;
        this.formatted = formatted;
        this.value = value;
        this.outputUnit = outputUnit;
    }

    @Override public String toString() { return text; }

    @Override public int length() { return text.length(); }

    @Override public char charAt(int index) { return text.charAt(index); }

    @Override public CharSequence subSequence(int start, int end) { return text.subSequence(start, end); }

    public <A extends Appendable> A appendTo(A appendable) {
        try {
            appendable.append(text);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return appendable;
    }

    public AttributedCharacterIterator toCharacterIterator() {
        if (format != null) {
            try {
                return format.formatToCharacterIterator(formatted);
            } catch (RuntimeException ignored) {
                /* the text without its fields */
            }
        }
        return new AttributedString(text).getIterator();
    }

    /* The number as it was shown: rounded, scaled, in the unit it was shown in. NaN and the infinities have none. */
    public BigDecimal toBigDecimal() {
        if (value == null) throw new ArithmeticException("Not a finite number: " + text);
        return value;
    }

    /* The unit the number is shown in: the unit for the formatter's usage in its locale, or the formatter's own. */
    public MeasureUnit getOutputUnit() { return outputUnit; }
}
