/*
 * Megler: methods Android 12 added to android.icu.util.MeasureUnit, which Android 9's core-libart (ICU 60) has without
 * them. A class of core-libart's has no sources here to add them to, so the runtime takes them from this class
 * (mg_dvm_class.c, "methods a boot class lacks"): a static method of the same name and signature is MeasureUnit's static
 * one, and one that takes a MeasureUnit first is the method of a MeasureUnit. AndroidX's LocalePreferences asks
 * getIdentifier() of the unit a temperature is shown in (Yandex Maps, Google Play services).
 */
package megler.members.android.icu.util;

public final class MeasureUnit {
    /* ICU 60's names that ICU 68's identifiers spell otherwise; the rest are the same. */
    private static final String[][] RENAMED = {
        { "liter-per-100kilometers", "liter-per-100-kilometer" },
        { "part-per-million", "permillion" },
        { "inch-hg", "inch-ofhg" },
        { "millimeter-of-mercury", "millimeter-ofhg" },
    };

    private MeasureUnit() {}

    /* "celsius", "kilometer-per-hour", "percent": the unit's name, "" for none; a currency's code. */
    public static String getIdentifier(android.icu.util.MeasureUnit unit) {
        String subtype = unit.getSubtype();
        if ("none".equals(unit.getType()) && "base".equals(subtype)) return "";
        for (String[] r : RENAMED)
            if (r[0].equals(subtype)) return r[1];
        return subtype;
    }

    public static android.icu.util.MeasureUnit forIdentifier(String identifier) {
        if (identifier == null || identifier.isEmpty()) return android.icu.util.NoUnit.BASE;
        for (android.icu.util.MeasureUnit u : android.icu.util.MeasureUnit.getAvailable())
            if (!"currency".equals(u.getType()) && identifier.equals(getIdentifier(u))) return u;
        throw new IllegalArgumentException("Unknown unit identifier: " + identifier);
    }
}
