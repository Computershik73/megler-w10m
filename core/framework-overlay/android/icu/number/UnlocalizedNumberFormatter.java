/* Megler: a formatter not yet given a locale (NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.ULocale;
import java.util.Locale;

public class UnlocalizedNumberFormatter extends NumberFormatterSettings<UnlocalizedNumberFormatter> {
    UnlocalizedNumberFormatter(Macros macros) { super(macros); }

    public LocalizedNumberFormatter locale(Locale locale) { return locale(locale == null ? null : ULocale.forLocale(locale)); }

    public LocalizedNumberFormatter locale(ULocale locale) {
        Macros m = macros.copy();
        m.locale = locale;
        return new LocalizedNumberFormatter(m);
    }

    @Override UnlocalizedNumberFormatter create(Macros macros) { return new UnlocalizedNumberFormatter(macros); }
}
