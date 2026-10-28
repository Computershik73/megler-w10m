package android.icu.text;

import android.icu.util.Currency;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public abstract class DecimalFormat extends NumberFormat {
    public DecimalFormatSymbols getDecimalFormatSymbols() { return null; }
    public void setDecimalFormatSymbols(DecimalFormatSymbols symbols) {}
    public void setCurrencyUsage(Currency.CurrencyUsage usage) {}
    public void setSignAlwaysShown(boolean shown) {}
    public void setMinimumGroupingDigits(int n) {}
    public void setGroupingSize(int n) {}
    public void setSecondaryGroupingSize(int n) {}
    public void setDecimalSeparatorAlwaysShown(boolean shown) {}
    public void setScientificNotation(boolean on) {}
    public void setMinimumExponentDigits(byte n) {}
    public void setExponentSignAlwaysShown(boolean shown) {}
    public void setSignificantDigitsUsed(boolean used) {}
    public boolean areSignificantDigitsUsed() { return false; }
    public void setMinimumSignificantDigits(int n) {}
    public void setMaximumSignificantDigits(int n) {}
    public int getMaximumSignificantDigits() { return 0; }
    public void setRoundingIncrement(java.math.BigDecimal increment) {}
    public java.math.BigDecimal getRoundingIncrement() { return null; }
    public void setMultiplier(int n) {}
    public String getPositivePrefix() { return null; }
    public String getPositiveSuffix() { return null; }
    public String getNegativePrefix() { return null; }
    public String getNegativeSuffix() { return null; }
    public String toPattern() { return null; }
    public void applyPattern(String pattern) {}
}
