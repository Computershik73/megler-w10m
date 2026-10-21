package android.icu.number;

import android.icu.util.Currency;
import android.icu.util.MeasureUnit;
import java.math.RoundingMode;
import java.text.DecimalFormat;
import java.text.NumberFormat;
import java.util.Locale;

public final class LocalizedNumberFormatter extends NumberFormatterSettings {
    private final Locale locale;

    LocalizedNumberFormatter(Locale locale, Precision precision, RoundingMode roundingMode,
                             MeasureUnit unit, NumberFormatter.UnitWidth unitWidth) {
        super(precision, roundingMode, unit, unitWidth);
        this.locale = locale == null ? Locale.getDefault() : locale;
    }

    public FormattedNumber format(double value) {
        boolean currency = unit instanceof Currency;
        NumberFormat formatter = currency && unitWidth != NumberFormatter.UnitWidth.HIDDEN
            ? NumberFormat.getCurrencyInstance(locale) : NumberFormat.getNumberInstance(locale);
        if (currency && unitWidth != NumberFormatter.UnitWidth.HIDDEN) {
            try { formatter.setCurrency(java.util.Currency.getInstance(((Currency) unit).getCurrencyCode())); }
            catch (IllegalArgumentException ignored) { /* Keep the locale's currency. */ }
        }
        if (precision != null) {
            formatter.setMinimumFractionDigits(precision.minFraction);
            formatter.setMaximumFractionDigits(precision.maxFraction);
        }
        if (roundingMode != null && formatter instanceof DecimalFormat)
            ((DecimalFormat) formatter).setRoundingMode(roundingMode);
        String result = formatter.format(value);
        if (unit != null && !currency && unitWidth != NumberFormatter.UnitWidth.HIDDEN)
            result += " " + unit.getSubtype();
        if (currency && unitWidth == NumberFormatter.UnitWidth.ISO_CODE)
            result = ((Currency) unit).getCurrencyCode() + " " + NumberFormat.getNumberInstance(locale).format(value);
        return new FormattedNumber(result);
    }
}
