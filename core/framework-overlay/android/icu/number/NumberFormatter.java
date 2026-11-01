/*
 * Megler: android.icu.number as Android 11 published it (ICU 66), with what Android 12 and 13 added to it (usage(),
 * FormattedNumber.getOutputUnit(), RoundingPriority, TrailingZeroDisplay). Android 9's core-libart has ICU 60's
 * android.icu.number, which Android had not published, which has other classes (Rounder, not Precision) and fewer
 * methods, and which its android.icu.text.DecimalFormat is made of; the runtime keeps those classes to core-libart
 * (mg_dvm_class.c, "packages a boot file keeps to itself"), and an app's android.icu.number is this one. It formats with
 * Android 9's android.icu.text -- DecimalFormat, CompactDecimalFormat, MeasureFormat -- so the locale's data is ICU 60's
 * (Formatting.java); a unit for a usage is chosen from CLDR's preferences of a few kinds of quantity (Units.java).
 * Not here: number skeletons (forSkeleton, toSkeleton), number ranges, mixed units ("foot-and-inch").
 */
package android.icu.number;

import android.icu.util.ULocale;
import java.util.Locale;

public final class NumberFormatter {
    private static final UnlocalizedNumberFormatter BASE = new UnlocalizedNumberFormatter(new Macros());

    public enum UnitWidth { NARROW, SHORT, FULL_NAME, ISO_CODE, FORMAL, VARIANT, HIDDEN }

    public enum SignDisplay { AUTO, ALWAYS, NEVER, ACCOUNTING, ACCOUNTING_ALWAYS, EXCEPT_ZERO, ACCOUNTING_EXCEPT_ZERO, NEGATIVE, ACCOUNTING_NEGATIVE }

    public enum DecimalSeparatorDisplay { AUTO, ALWAYS }

    public enum GroupingStrategy { OFF, MIN2, AUTO, ON_ALIGNED, THOUSANDS }

    public enum RoundingPriority { RELAXED, STRICT }

    public enum TrailingZeroDisplay { AUTO, HIDE_IF_WHOLE }

    private NumberFormatter() {}

    public static UnlocalizedNumberFormatter with() { return BASE; }

    public static LocalizedNumberFormatter withLocale(Locale locale) { return BASE.locale(locale); }

    public static LocalizedNumberFormatter withLocale(ULocale locale) { return BASE.locale(locale); }
}
