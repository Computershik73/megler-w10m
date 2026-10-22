package android.icu.number;

import android.icu.util.MeasureUnit;
import java.math.RoundingMode;
import java.util.Locale;

public final class UnlocalizedNumberFormatter extends NumberFormatterSettings {
    UnlocalizedNumberFormatter() { this(null, null, null, NumberFormatter.UnitWidth.SHORT); }

    private UnlocalizedNumberFormatter(Precision precision, RoundingMode roundingMode, MeasureUnit unit,
                                       NumberFormatter.UnitWidth unitWidth) {
        super(precision, roundingMode, unit, unitWidth);
    }

    public NumberFormatterSettings precision(Precision value) {
        return new UnlocalizedNumberFormatter(value, roundingMode, unit, unitWidth);
    }

    public NumberFormatterSettings roundingMode(RoundingMode value) {
        return new UnlocalizedNumberFormatter(precision, value, unit, unitWidth);
    }

    public NumberFormatterSettings unit(MeasureUnit value) {
        return new UnlocalizedNumberFormatter(precision, roundingMode, value, unitWidth);
    }

    public NumberFormatterSettings unitWidth(NumberFormatter.UnitWidth value) {
        return new UnlocalizedNumberFormatter(precision, roundingMode, unit, value);
    }

    public LocalizedNumberFormatter locale(Locale value) {
        return new LocalizedNumberFormatter(value, precision, roundingMode, unit, unitWidth);
    }
}
