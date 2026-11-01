/* Megler: Android 13's settings of a number formatter (NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.text.DecimalFormatSymbols;
import android.icu.text.NumberingSystem;
import android.icu.util.MeasureUnit;
import java.math.RoundingMode;

public abstract class NumberFormatterSettings<T extends NumberFormatterSettings<?>> {
    final Macros macros;

    NumberFormatterSettings(Macros macros) { this.macros = macros; }

    /* A formatter of the same kind with these settings. */
    abstract T create(Macros macros);

    public T notation(Notation notation) { Macros m = macros.copy(); m.notation = notation; return create(m); }

    public T unit(MeasureUnit unit) { Macros m = macros.copy(); m.unit = unit; return create(m); }

    public T perUnit(MeasureUnit perUnit) { Macros m = macros.copy(); m.perUnit = perUnit; return create(m); }

    public T precision(Precision precision) { Macros m = macros.copy(); m.precision = precision; return create(m); }

    public T roundingMode(RoundingMode roundingMode) { Macros m = macros.copy(); m.roundingMode = roundingMode; return create(m); }

    public T grouping(NumberFormatter.GroupingStrategy strategy) { Macros m = macros.copy(); m.grouping = strategy; return create(m); }

    public T integerWidth(IntegerWidth style) { Macros m = macros.copy(); m.integerWidth = style; return create(m); }

    public T symbols(DecimalFormatSymbols symbols) {
        Macros m = macros.copy();
        m.symbols = symbols == null ? null : symbols.clone();
        return create(m);
    }

    public T symbols(NumberingSystem ns) { Macros m = macros.copy(); m.symbols = ns; return create(m); }

    public T unitWidth(NumberFormatter.UnitWidth style) { Macros m = macros.copy(); m.unitWidth = style; return create(m); }

    public T sign(NumberFormatter.SignDisplay style) { Macros m = macros.copy(); m.sign = style; return create(m); }

    public T decimal(NumberFormatter.DecimalSeparatorDisplay style) { Macros m = macros.copy(); m.decimal = style; return create(m); }

    public T scale(Scale scale) { Macros m = macros.copy(); m.scale = scale; return create(m); }

    public T usage(String usage) { Macros m = macros.copy(); m.usage = usage == null || usage.isEmpty() ? null : usage; return create(m); }

    @Override public int hashCode() { return macros.hashCode(); }

    @Override public boolean equals(Object other) {
        return other != null && other.getClass() == getClass() && macros.equals(((NumberFormatterSettings<?>) other).macros);
    }
}
