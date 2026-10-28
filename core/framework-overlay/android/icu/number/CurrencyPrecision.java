/* Megler: rounding as a currency is (NumberFormatter.java says why this package is here). */
package android.icu.number;

import android.icu.util.Currency;

public class CurrencyPrecision extends Precision {
    CurrencyPrecision(Currency.CurrencyUsage usage, Currency currency) { super(CURRENCY, -1, -1, -1, -1, null, null, usage, currency, null); }

    /* As this currency is rounded, rather than the formatter's unit. */
    public Precision withCurrency(Currency currency) {
        if (currency == null) throw new IllegalArgumentException("Currency must not be null");
        return new CurrencyPrecision(usage, currency);
    }
}
