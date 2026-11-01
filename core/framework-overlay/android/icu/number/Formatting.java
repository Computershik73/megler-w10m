/*
 * Megler: formatting a number as a formatter's settings say, with Android 9's android.icu.text (ICU 60), which has what
 * it takes: DecimalFormat with significant digits, rounding increments, minimum grouping digits, the sign always shown;
 * CompactDecimalFormat; MeasureFormat, given that DecimalFormat for the number. Rounding is DecimalFormat's (a fraction
 * rounding that counts significant digits too is worked out for the value at hand, as ICU does); the unit is first the
 * one for the usage (Units.java), and the number scaled. NumberFormatter.java says why this package is here.
 */
package android.icu.number;

import android.icu.text.CompactDecimalFormat;
import android.icu.text.DecimalFormat;
import android.icu.text.DecimalFormatSymbols;
import android.icu.text.MeasureFormat;
import android.icu.text.NumberFormat;
import android.icu.text.NumberingSystem;
import android.icu.util.Currency;
import android.icu.util.Measure;
import android.icu.util.MeasureUnit;
import android.icu.util.NoUnit;
import android.icu.util.ULocale;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.MathContext;
import java.math.RoundingMode;
import java.text.FieldPosition;
import java.text.Format;

final class Formatting {
    /* As many fraction digits as there may be: what "no limit" is to DecimalFormat (a double has fewer). */
    private static final int ALL_FRACTION_DIGITS = 340;

    private Formatting() {}

    static FormattedNumber format(Macros m, Number input) {
        ULocale locale = m.locale != null ? m.locale : ULocale.getDefault();
        if (m.symbols instanceof NumberingSystem) locale = locale.setKeywordValue("numbers", ((NumberingSystem) m.symbols).getName());
        MeasureUnit unit = m.unit;
        BigDecimal value = exact(input);
        if (value != null && unit != null && m.usage != null) {
            Units.Preferred p = Units.preferred(unit, m.usage, locale, value);
            if (p != null) {
                unit = p.unit;
                value = p.value;
            }
        }
        if (value != null && m.scale != null) value = m.scale.applyTo(value);
        RoundingMode mode = m.roundingMode != null ? m.roundingMode : RoundingMode.HALF_EVEN;
        NumberFormatter.UnitWidth width = m.unitWidth != null ? m.unitWidth : NumberFormatter.UnitWidth.SHORT;
        NumberFormatter.SignDisplay sign = m.sign != null ? m.sign : NumberFormatter.SignDisplay.AUTO;
        String type = unit == null ? "none" : unit.getType(), subtype = unit == null ? "base" : unit.getSubtype();
        Currency currency = unit instanceof Currency ? (Currency) unit : null;
        boolean percent = currency == null && "percent".equals(subtype), permille = currency == null && "permille".equals(subtype);
        boolean measure = unit != null && currency == null && !percent && !permille && !"none".equals(type);
        boolean compact = m.notation instanceof CompactNotation;
        ScientificNotation scientific = m.notation instanceof ScientificNotation ? (ScientificNotation) m.notation : null;
        boolean showCurrency = currency != null && width != NumberFormatter.UnitWidth.HIDDEN;
        boolean accounting = sign == NumberFormatter.SignDisplay.ACCOUNTING || sign == NumberFormatter.SignDisplay.ACCOUNTING_ALWAYS
            || sign == NumberFormatter.SignDisplay.ACCOUNTING_EXCEPT_ZERO || sign == NumberFormatter.SignDisplay.ACCOUNTING_NEGATIVE;

        NumberFormat nf;
        if (compact) {
            nf = CompactDecimalFormat.getInstance(locale, ((CompactNotation) m.notation).longNames ? CompactDecimalFormat.CompactStyle.LONG
                : CompactDecimalFormat.CompactStyle.SHORT);
        } else {
            int style = NumberFormat.NUMBERSTYLE;
            if (showCurrency) {
                style = width == NumberFormatter.UnitWidth.ISO_CODE ? NumberFormat.ISOCURRENCYSTYLE : width == NumberFormatter.UnitWidth.FULL_NAME
                    ? NumberFormat.PLURALCURRENCYSTYLE : accounting ? NumberFormat.ACCOUNTINGCURRENCYSTYLE : NumberFormat.CURRENCYSTYLE;
            } else if (percent || permille) {
                style = NumberFormat.PERCENTSTYLE;
            }
            nf = NumberFormat.getInstance(locale, style);
        }
        /* A numbering system that is not of digits (a rule-based one) gives another kind of NumberFormat, which takes less of this. */
        DecimalFormat df = nf instanceof DecimalFormat ? (DecimalFormat) nf : null;
        if (df != null && m.symbols instanceof DecimalFormatSymbols) df.setDecimalFormatSymbols((DecimalFormatSymbols) ((DecimalFormatSymbols) m.symbols).clone());
        if (showCurrency && !compact) nf.setCurrency(currency);
        if (df != null && permille) df.applyPattern(df.toPattern().replace('%', '‰'));
        /* The unit's own number: 50 percent is 50%, not 5000%. */
        if (df != null && (percent || permille)) df.setMultiplier(1);
        if (df != null && scientific != null) {
            df.setScientificNotation(true);
            df.setMaximumIntegerDigits(scientific.engineeringInterval);
            df.setMinimumIntegerDigits(1);
            df.setMinimumExponentDigits((byte) Math.min(scientific.minExponentDigits, 127));
            df.setExponentSignAlwaysShown(scientific.exponentSign == NumberFormatter.SignDisplay.ALWAYS || scientific.exponentSign == NumberFormatter.SignDisplay.EXCEPT_ZERO);
        }
        precision(nf, df, m.precision, value, currency, compact, mode);
        /* RoundingMode's order is BigDecimal's ROUND_ constants', which DecimalFormat takes. */
        nf.setRoundingMode(mode.ordinal());
        if (m.grouping != null && df != null) {
            switch (m.grouping) {
            case OFF: df.setGroupingUsed(false); break;
            case MIN2: df.setGroupingUsed(true); df.setMinimumGroupingDigits(2); break;
            case ON_ALIGNED: df.setGroupingUsed(true); df.setMinimumGroupingDigits(1); break;
            case THOUSANDS: df.setGroupingUsed(true); df.setGroupingSize(3); df.setSecondaryGroupingSize(3); df.setMinimumGroupingDigits(1); break;
            default: break;
            }
        }
        if (m.integerWidth != null && scientific == null) {
            nf.setMinimumIntegerDigits(m.integerWidth.minInt);
            if (m.integerWidth.maxInt >= 0) nf.setMaximumIntegerDigits(m.integerWidth.maxInt);
        }
        if (df != null && m.decimal == NumberFormatter.DecimalSeparatorDisplay.ALWAYS) df.setDecimalSeparatorAlwaysShown(true);

        Number shown = value != null ? value : input;
        boolean always = sign == NumberFormatter.SignDisplay.ALWAYS || sign == NumberFormatter.SignDisplay.ACCOUNTING_ALWAYS;
        if (sign == NumberFormatter.SignDisplay.NEVER) {
            shown = abs(shown);
        } else if (sign == NumberFormatter.SignDisplay.EXCEPT_ZERO || sign == NumberFormatter.SignDisplay.ACCOUNTING_EXCEPT_ZERO
                   || sign == NumberFormatter.SignDisplay.NEGATIVE || sign == NumberFormatter.SignDisplay.ACCOUNTING_NEGATIVE) {
            /* Zero once rounded is shown without a sign: what it is shown as is the number's own, rounded as it will be. */
            boolean zero = nf.format(abs(shown)).equals(nf.format(BigDecimal.ZERO));
            if (zero) shown = abs(shown);
            else if (sign == NumberFormatter.SignDisplay.EXCEPT_ZERO || sign == NumberFormatter.SignDisplay.ACCOUNTING_EXCEPT_ZERO) always = true;
        }
        if (always && df != null) df.setSignAlwaysShown(true);

        String text;
        Format alone = null;
        if (measure && width != NumberFormatter.UnitWidth.HIDDEN) {
            MeasureFormat.FormatWidth w = width == NumberFormatter.UnitWidth.FULL_NAME ? MeasureFormat.FormatWidth.WIDE
                : width == NumberFormatter.UnitWidth.NARROW ? MeasureFormat.FormatWidth.NARROW : MeasureFormat.FormatWidth.SHORT;
            Measure amount = new Measure(shown, unit);
            try {
                MeasureFormat mf = MeasureFormat.getInstance(locale, w, nf);
                text = m.perUnit != null ? mf.formatMeasurePerUnit(amount, m.perUnit, new StringBuilder(), new FieldPosition(0)).toString() : mf.format(amount);
            } catch (RuntimeException e) {
                text = nf.format(shown) + " " + subtype;
            }
        } else if (showCurrency && compact) {
            text = compactCurrency(nf, locale, currency, shown);
        } else {
            text = nf.format(shown);
            alone = nf;
        }
        BigDecimal rounded = value == null ? null : rounded(nf, df, sign == NumberFormatter.SignDisplay.NEVER ? value.abs() : value, mode, !compact && scientific == null);
        return new FormattedNumber(text, alone, shown, rounded, unit != null ? unit : NoUnit.BASE);
    }

    /* The number exactly, as a decimal: a double as its shortest form (0.1, not 0.1000000000000000055511151231257827). NaN and the infinities: null. */
    static BigDecimal exact(Number n) {
        if (n instanceof BigDecimal) return (BigDecimal) n;
        if (n instanceof BigInteger) return new BigDecimal((BigInteger) n);
        if (n instanceof Long || n instanceof Integer || n instanceof Short || n instanceof Byte) return BigDecimal.valueOf(n.longValue());
        if (n instanceof Double || n instanceof Float) {
            double d = n.doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) return null;
            return n instanceof Float ? new BigDecimal(n.toString()) : BigDecimal.valueOf(d);
        }
        try {
            return new BigDecimal(n.toString());
        } catch (NumberFormatException e) {
            double d = n.doubleValue();
            return Double.isNaN(d) || Double.isInfinite(d) ? null : BigDecimal.valueOf(d);
        }
    }

    private static Number abs(Number n) {
        if (n instanceof BigDecimal) return ((BigDecimal) n).abs();
        double d = n.doubleValue();
        return Double.isNaN(d) ? n : Double.valueOf(Math.abs(d));
    }

    private static void fraction(NumberFormat nf, DecimalFormat df, int min, int max) {
        if (df != null) df.setSignificantDigitsUsed(false);
        nf.setMaximumFractionDigits(max);
        nf.setMinimumFractionDigits(min);
    }

    /* The precision into the format: none given is six fraction digits at most, a currency's own digits, or the compact format's own rounding. */
    private static void precision(NumberFormat nf, DecimalFormat df, Precision p, BigDecimal value, Currency currency, boolean compact, RoundingMode mode) {
        if (p == null) {
            if (compact) return;
            p = currency != null ? Precision.currency(Currency.CurrencyUsage.STANDARD) : Precision.maxFraction(6);
        }
        int all = value == null ? ALL_FRACTION_DIGITS : Math.max(ALL_FRACTION_DIGITS, value.scale());
        switch (p.kind) {
        case Precision.UNLIMITED:
            fraction(nf, df, 0, all);
            break;
        case Precision.FRACTION:
            fraction(nf, df, p.minFraction, p.maxFraction < 0 ? all : p.maxFraction);
            break;
        case Precision.SIGNIFICANT:
            if (df == null) break;
            df.setSignificantDigitsUsed(true);
            df.setMaximumSignificantDigits(p.maxSignificant < 0 ? 999 : p.maxSignificant);
            df.setMinimumSignificantDigits(p.minSignificant);
            break;
        case Precision.FRACTION_SIGNIFICANT: {
            /* The magnitude rounded to: the fraction's, or the significant digits' for this value, whichever the precision says. */
            int round = -(p.maxFraction < 0 ? all : p.maxFraction);
            if (value != null && value.signum() != 0) {
                int top = value.precision() - value.scale() - 1;
                if (p.priority == null && p.maxSignificant < 0) round = Math.min(round, top - p.minSignificant + 1);
                else if (p.priority == null) round = Math.max(round, top - p.maxSignificant + 1);
                else if (p.priority == NumberFormatter.RoundingPriority.RELAXED) round = Math.min(round, top - p.maxSignificant + 1);
                else round = Math.max(round, top - p.maxSignificant + 1);
            }
            int digits = Math.max(0, -round);
            fraction(nf, df, Math.min(p.minFraction, digits), digits);
            if (round > 0 && df != null) df.setRoundingIncrement(BigDecimal.ONE.scaleByPowerOfTen(round));
            break;
        }
        case Precision.INCREMENT: {
            int digits = Math.max(0, p.increment.scale());
            fraction(nf, df, digits, digits);
            if (df != null) df.setRoundingIncrement(p.increment);
            break;
        }
        case Precision.CURRENCY: {
            Currency c = p.currency != null ? p.currency : currency;
            if (c == null) {
                fraction(nf, df, 2, 2);
                break;
            }
            int digits = c.getDefaultFractionDigits(p.usage);
            double increment = c.getRoundingIncrement(p.usage);
            fraction(nf, df, digits, digits);
            if (increment > 0 && df != null) df.setRoundingIncrement(BigDecimal.valueOf(increment));
            break;
        }
        default:
            break;
        }
        if (p.trailingZeros == NumberFormatter.TrailingZeroDisplay.HIDE_IF_WHOLE && value != null) {
            BigDecimal r = rounded(nf, df, value, mode, true);
            if (r.scale() <= 0) {
                nf.setMinimumFractionDigits(0);
                if (df != null && df.areSignificantDigitsUsed()) df.setMinimumSignificantDigits(1);
            }
        }
    }

    /*
     * The value as it is shown, once rounded as the format rounds: `plain` when the format rounds the number itself (not a
     * compact or scientific one, which rounds what is left of it), without zeros at its end.
     */
    static BigDecimal rounded(NumberFormat nf, DecimalFormat df, BigDecimal v, RoundingMode mode, boolean plain) {
        BigDecimal r = v;
        if (plain) {
            try {
                BigDecimal increment = df != null ? df.getRoundingIncrement() : null;
                if (increment != null && increment.signum() > 0) r = v.divide(increment, 0, mode).multiply(increment);
                else if (df != null && df.areSignificantDigitsUsed()) r = v.signum() == 0 ? v : v.round(new MathContext(df.getMaximumSignificantDigits(), mode));
                else r = v.setScale(nf.getMaximumFractionDigits(), mode);
            } catch (ArithmeticException e) {
                r = v;                 /* RoundingMode.UNNECESSARY, and the format says so as it formats */
            }
        }
        r = r.signum() == 0 ? BigDecimal.ZERO : r.stripTrailingZeros();
        return r.scale() < 0 ? r.setScale(0) : r;
    }

    /* A compact number of a currency: the compact format's number, between the currency format's prefix and suffix. */
    private static String compactCurrency(NumberFormat compact, ULocale locale, Currency currency, Number shown) {
        NumberFormat cf = NumberFormat.getInstance(locale, NumberFormat.CURRENCYSTYLE);
        if (!(cf instanceof DecimalFormat)) return compact.format(shown);
        DecimalFormat c = (DecimalFormat) cf;
        c.setCurrency(currency);
        boolean negative = shown instanceof BigDecimal ? ((BigDecimal) shown).signum() < 0 : shown.doubleValue() < 0;
        String body = compact.format(abs(shown));
        return negative ? c.getNegativePrefix() + body + c.getNegativeSuffix() : c.getPositivePrefix() + body + c.getPositiveSuffix();
    }
}
