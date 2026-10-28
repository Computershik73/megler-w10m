package android.icu.text;

import android.icu.util.ULocale;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public abstract class CompactDecimalFormat extends DecimalFormat {
    public enum CompactStyle { SHORT, LONG }
    public static CompactDecimalFormat getInstance(ULocale locale, CompactStyle style) { return null; }
}
