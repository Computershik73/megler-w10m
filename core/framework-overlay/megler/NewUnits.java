/*
 * Megler: the units of measurement that Android 11 and later have and Android 9's ICU (60, in core-libart) does not:
 * MeasureUnit.PERCENT, ATMOSPHERE, KILOPASCAL and the rest. megler.members.android.icu.util.MeasureUnit holds the
 * constants (the runtime gives an app that reads MeasureUnit.PERCENT that class's field); this class makes the units and
 * gives ICU their words.
 *
 * ICU 60 can make a unit of any name (MeasureUnit.internalGetInstance), and a unit made so is among getAvailable()'s.
 * What it has no means to learn is how to write one: MeasureFormat takes a unit's patterns ("{0} km") and its name from
 * the locale's unit data, where a unit that CLDR 32 had not yet is absent, so format() of one threw and
 * getUnitDisplayName() said null (Breezy Weather, which is Kotlin, stopped on that null). The data of a locale is loaded
 * once and kept in a cache that is a private static of MeasureFormat's; install() puts a cache of its own there, which
 * adds the new units to each locale's data as it is stored:
 *
 *   - a unit that is another under a new name (TONNE is METRIC_TON, DAY_PERSON is written as DAY) takes that unit's
 *     words, in the locale's language;
 *   - percent, permille and permyriad are written as the locale writes a percentage ("{0}%", "{0} %", "%{0}"), with the
 *     locale's own sign;
 *   - the rest are written with their sign, which is the same in most languages ("{0} atm", "{0} kPa"), and in English at
 *     full width with their names ("{0} atmospheres"). Nothing here has the names in other languages.
 *
 * All of it is by reflection on names that are ICU 60's; whatever fails leaves ICU as it was.
 */
package megler;

import android.icu.text.DecimalFormat;
import android.icu.text.DecimalFormatSymbols;
import android.icu.text.MeasureFormat;
import android.icu.text.MeasureFormat.FormatWidth;
import android.icu.text.NumberFormat;
import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;
import java.lang.ref.Reference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.EnumMap;
import java.util.Map;

public final class NewUnits {
    /*
     * Type, subtype, then either the sign, the English name of one and of several, or the type and subtype of the unit
     * whose words are this one's.
     */
    private static final String[][] UNITS = {
        { "concentr", "percent", "%", "percent", "percent" },
        { "concentr", "permille", "‰", "permille", "permille" },
        { "concentr", "permyriad", "‱", "permyriad", "permyriad" },
        { "concentr", "item", "item", "item", "items" },
        { "concentr", "mole", "mol", "mole", "moles" },
        { "concentr", "milligram-ofglucose-per-deciliter", "concentr", "milligram-per-deciliter" },
        { "pressure", "atmosphere", "atm", "atmosphere", "atmospheres" },
        { "pressure", "bar", "bar", "bar", "bars" },
        { "pressure", "pascal", "Pa", "pascal", "pascals" },
        { "pressure", "kilopascal", "kPa", "kilopascal", "kilopascals" },
        { "pressure", "megapascal", "MPa", "megapascal", "megapascals" },
        { "digital", "petabyte", "PB", "petabyte", "petabytes" },
        { "energy", "british-thermal-unit", "Btu", "British thermal unit", "British thermal units" },
        { "energy", "electronvolt", "eV", "electronvolt", "electronvolts" },
        { "energy", "therm-us", "US therm", "US therm", "US therms" },
        { "force", "newton", "N", "newton", "newtons" },
        { "force", "pound-force", "lbf", "pound of force", "pounds of force" },
        { "torque", "newton-meter", "N⋅m", "newton-meter", "newton-meters" },
        { "torque", "pound-force-foot", "lbf⋅ft", "pound-force-foot", "pound-feet" },
        { "light", "candela", "cd", "candela", "candela" },
        { "light", "lumen", "lm", "lumen", "lumen" },
        { "light", "solar-luminosity", "L☉", "solar luminosity", "solar luminosities" },
        { "mass", "dalton", "Da", "dalton", "daltons" },
        { "mass", "earth-mass", "M⊕", "Earth mass", "Earth masses" },
        { "mass", "solar-mass", "M☉", "solar mass", "solar masses" },
        { "mass", "grain", "gr", "grain", "grains" },
        { "mass", "tonne", "mass", "metric-ton" },
        { "length", "earth-radius", "R⊕", "earth radius", "earth radius" },
        { "length", "solar-radius", "R☉", "solar radius", "solar radii" },
        { "area", "dunam", "dunam", "dunam", "dunams" },
        { "duration", "decade", "dec", "decade", "decades" },
        { "duration", "quarter", "qtr", "quarter", "quarters" },
        { "duration", "day-person", "duration", "day" },
        { "duration", "week-person", "duration", "week" },
        { "duration", "month-person", "duration", "month" },
        { "duration", "year-person", "duration", "year" },
        { "volume", "barrel", "bbl", "barrel", "barrels" },
        { "volume", "dessert-spoon", "dstspn", "dessert spoon", "dessert spoons" },
        { "volume", "dessert-spoon-imperial", "dstspn Imp", "Imp. dessert spoon", "Imp. dessert spoons" },
        { "volume", "dram", "dram fl", "dram", "drams" },
        { "volume", "drop", "dr", "drop", "drops" },
        { "volume", "jigger", "jigger", "jigger", "jiggers" },
        { "volume", "pinch", "pn", "pinch", "pinches" },
        { "volume", "quart-imperial", "qt Imp", "Imp. quart", "Imp. quarts" },
        { "volume", "fluid-ounce-imperial", "fl oz Imp.", "Imp. fluid ounce", "Imp. fluid ounces" },
        { "graphics", "dot", "dot", "dot", "dots" },
        { "graphics", "dot-per-centimeter", "dpcm", "dot per centimeter", "dots per centimeter" },
        { "graphics", "dot-per-inch", "dpi", "dot per inch", "dots per inch" },
        { "graphics", "em", "em", "em", "ems" },
        { "graphics", "megapixel", "MP", "megapixel", "megapixels" },
        { "graphics", "pixel", "px", "pixel", "pixels" },
        { "graphics", "pixel-per-centimeter", "ppcm", "pixel per centimeter", "pixels per centimeter" },
        { "graphics", "pixel-per-inch", "ppi", "pixel per inch", "pixels per inch" },
        { "consumption", "kilowatt-hour-per-100-kilometer", "kWh/100km", "kilowatt-hour per 100 kilometers", "kilowatt-hours per 100 kilometers" },
    };
    private static final FormatWidth[] WIDTHS = { FormatWidth.WIDE, FormatWidth.SHORT, FormatWidth.NARROW };

    private static boolean asked;
    private static Field patternsOf, namesOf;       /* MeasureFormatData's maps: unit -> width -> patterns, -> name */
    private static Method compile;                  /* SimpleFormatterImpl's: "{0} km" as MeasureFormat keeps it */
    private static int patternCount, one, other;    /* a unit's patterns: one for each plural form, and the "per" one */

    private NewUnits() {}

    /* The unit of this type and name, which ICU knows how to write from now on. */
    public static MeasureUnit unit(String type, String subtype) {
        install();
        return MeasureUnit.internalGetInstance(type, subtype);
    }

    /* MeasureFormat's cache of the locales' unit data, which adds the new units to what it is given to keep. */
    private static final class Cache extends android.icu.impl.SimpleCache {
        @Override public void put(Object locale, Object data) {
            add(locale, data);
            super.put(locale, data);
        }
    }

    private static synchronized void install() {
        if (asked) return;
        asked = true;
        try {
            Class<?> data = Class.forName("android.icu.text.MeasureFormat$MeasureFormatData");
            Class<?> plural = Class.forName("android.icu.impl.StandardPlural");
            Field count = data.getDeclaredField("PATTERN_COUNT"), patterns = data.getDeclaredField("unitToStyleToPatterns"),
                names = data.getDeclaredField("unitToStyleToDnam"), cache = MeasureFormat.class.getDeclaredField("localeMeasureFormatData");
            count.setAccessible(true);
            patterns.setAccessible(true);
            names.setAccessible(true);
            cache.setAccessible(true);
            int n = count.getInt(null), o = ordinal(plural, "OTHER"), i = ordinal(plural, "ONE");
            if (o >= n || i >= n) throw new IllegalStateException("a unit has " + n + " patterns");
            compile = Class.forName("android.icu.impl.SimpleFormatterImpl")
                .getMethod("compileToStringMinMaxArguments", CharSequence.class, StringBuilder.class, int.class, int.class);
            patternCount = n;
            other = o;
            one = i;
            patternsOf = patterns;
            namesOf = names;
            Cache mine = new Cache();
            Object old = cache.get(null);
            cache.set(null, mine);
            /* The locales loaded before this: their data gets the units too, for the formats made of it already. */
            Field ref = android.icu.impl.SimpleCache.class.getDeclaredField("cacheRef");
            ref.setAccessible(true);
            Object r = old == null ? null : ref.get(old), kept = r instanceof Reference ? ((Reference<?>) r).get() : null;
            if (kept instanceof Map) {
                synchronized (kept) {
                    for (Map.Entry<?, ?> e : ((Map<?, ?>) kept).entrySet()) mine.put(e.getKey(), e.getValue());
                }
            }
        } catch (Throwable e) {
            android.util.Log.w("Megler", "the units Android 9 has no words for stay without them: " + e);
        }
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private static int ordinal(Class<?> of, String name) {
        return Enum.valueOf((Class) of, name).ordinal();
    }

    private static String compiled(String pattern) throws Exception {
        return (String) compile.invoke(null, pattern, new StringBuilder(), 1, 1);
    }

    /* The new units into a locale's data, where the data has nothing of a unit. */
    @SuppressWarnings("unchecked")
    private static void add(Object locale, Object data) {
        if (patternsOf == null || data == null) return;
        try {
            Map<Object, Object> patterns = (Map<Object, Object>) patternsOf.get(data), names = (Map<Object, Object>) namesOf.get(data);
            boolean english = locale instanceof ULocale && "en".equals(((ULocale) locale).getLanguage());
            for (String[] u : UNITS) {
                MeasureUnit unit = MeasureUnit.internalGetInstance(u[0], u[1]);
                if (patterns.containsKey(unit)) continue;
                if (u.length == 4) {
                    MeasureUnit like = MeasureUnit.internalGetInstance(u[2], u[3]);
                    Object p = patterns.get(like), n = names.get(like);
                    if (p != null) patterns.put(unit, p);
                    if (n != null && !names.containsKey(unit)) names.put(unit, n);
                    continue;
                }
                boolean ratio = "%".equals(u[2]) || "‰".equals(u[2]) || "‱".equals(u[2]);
                String[] written = ratio ? ratio(locale, u[2]) : new String[] { "{0} " + u[2], u[2] };
                EnumMap<FormatWidth, String[]> p = new EnumMap<FormatWidth, String[]>(FormatWidth.class);
                EnumMap<FormatWidth, String> n = new EnumMap<FormatWidth, String>(FormatWidth.class);
                for (FormatWidth w : WIDTHS) {
                    boolean words = english && w == FormatWidth.WIDE;
                    String[] forms = new String[patternCount];
                    forms[other] = compiled(words ? "{0} " + u[4] : written[0]);
                    if (words) forms[one] = compiled("{0} " + u[3]);
                    p.put(w, forms);
                    n.put(w, words ? u[4] : written[1]);
                }
                patterns.put(unit, p);
                if (!names.containsKey(unit)) names.put(unit, n);
            }
        } catch (Throwable e) {
            android.util.Log.w("Megler", "no words for the new units in " + locale + ": " + e);
        }
    }

    /*
     * How the locale writes a number with this sign, and the sign as the locale has it: the place of the percent sign in
     * the locale's percent pattern ("#,##0%", "#,##0 %", "%#,##0") is the sign's place, and the locale's symbols have its
     * own percent and per mille signs. "{0}" and the sign after it when the pattern is not one this reads.
     */
    private static String[] ratio(Object locale, String sign) {
        String before = "", after = null;
        try {
            ULocale l = (ULocale) locale;
            DecimalFormatSymbols symbols = DecimalFormatSymbols.getInstance(l);
            String own = "%".equals(sign) ? symbols.getPercentString() : "‰".equals(sign) ? symbols.getPerMillString() : sign;
            if (own != null && !own.isEmpty() && own.indexOf('\'') < 0 && own.indexOf('{') < 0 && own.indexOf('}') < 0) sign = own;
            NumberFormat f = NumberFormat.getInstance(l, NumberFormat.PERCENTSTYLE);
            String p = f instanceof DecimalFormat ? ((DecimalFormat) f).toPattern() : "";
            int end = p.indexOf(';'), first = -1, last = -1;
            if (end >= 0) p = p.substring(0, end);
            for (int i = 0; i < p.length(); i++) {
                if ("#0,.@".indexOf(p.charAt(i)) < 0) continue;
                if (first < 0) first = i;
                last = i;
            }
            if (first >= 0 && p.indexOf('%') >= 0 && p.indexOf('\'') < 0 && p.indexOf('{') < 0 && p.indexOf('}') < 0) {
                before = p.substring(0, first).replace("%", sign);
                after = p.substring(last + 1).replace("%", sign);
            }
        } catch (RuntimeException e) {
            before = "";
            after = null;
        }
        return new String[] { before + "{0}" + (after == null ? sign : after), sign };
    }
}
