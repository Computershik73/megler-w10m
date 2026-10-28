package android.icu.text;

import android.icu.util.Currency;
import android.icu.util.ULocale;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public abstract class NumberFormat extends java.text.Format {
    public static final int NUMBERSTYLE = 0;
    public static final int CURRENCYSTYLE = 1;
    public static final int PERCENTSTYLE = 2;
    public static final int SCIENTIFICSTYLE = 3;
    public static final int INTEGERSTYLE = 4;
    public static final int ISOCURRENCYSTYLE = 5;
    public static final int PLURALCURRENCYSTYLE = 6;
    public static final int ACCOUNTINGCURRENCYSTYLE = 7;
    public static final int CASHCURRENCYSTYLE = 8;
    public static final int STANDARDCURRENCYSTYLE = 9;
    public static NumberFormat getInstance(ULocale locale, int style) { return null; }
    public void setCurrency(Currency currency) {}
    public void setGroupingUsed(boolean used) {}
    public void setMaximumFractionDigits(int n) {}
    public void setMinimumFractionDigits(int n) {}
    public void setMaximumIntegerDigits(int n) {}
    public void setMinimumIntegerDigits(int n) {}
    public int getMaximumFractionDigits() { return 0; }
    public int getMinimumFractionDigits() { return 0; }
    public void setRoundingMode(int mode) {}
}
