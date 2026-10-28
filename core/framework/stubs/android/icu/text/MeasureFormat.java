package android.icu.text;

import android.icu.util.Measure;
import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;

/** Compile-only declarations: Android 9's core-libart.dex (ICU 60) has these classes at run time, with these signatures. */
public abstract class MeasureFormat extends java.text.Format {
    public enum FormatWidth { WIDE, SHORT, NARROW, NUMERIC }
    public static MeasureFormat getInstance(ULocale locale, FormatWidth width, NumberFormat format) { return null; }
    public StringBuilder formatMeasurePerUnit(Measure measure, MeasureUnit perUnit, StringBuilder appendTo, java.text.FieldPosition pos) { return null; }
}
