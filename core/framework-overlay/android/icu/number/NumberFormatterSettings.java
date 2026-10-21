package android.icu.number;

import android.icu.util.MeasureUnit;
import java.math.RoundingMode;

/** Immutable settings shared by number formatters. */
public class NumberFormatterSettings {
    final Precision precision;
    final RoundingMode roundingMode;
    final MeasureUnit unit;
    final NumberFormatter.UnitWidth unitWidth;

    NumberFormatterSettings(Precision precision, RoundingMode roundingMode, MeasureUnit unit,
                            NumberFormatter.UnitWidth unitWidth) {
        this.precision = precision;
        this.roundingMode = roundingMode;
        this.unit = unit;
        this.unitWidth = unitWidth;
    }
}
