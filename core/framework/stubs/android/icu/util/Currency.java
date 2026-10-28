package android.icu.util;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public class Currency extends MeasureUnit {
    public enum CurrencyUsage { STANDARD, CASH }
    Currency() {}
    public static Currency getInstance(String code) { return null; }
    public String getCurrencyCode() { return null; }
    public int getDefaultFractionDigits(CurrencyUsage usage) { return 0; }
    public double getRoundingIncrement(CurrencyUsage usage) { return 0; }
}
