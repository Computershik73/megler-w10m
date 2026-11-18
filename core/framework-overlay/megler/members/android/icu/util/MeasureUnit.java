/*
 * Megler: methods Android 12 added to android.icu.util.MeasureUnit, which Android 9's core-libart (ICU 60) has without
 * them. A class of core-libart's has no sources here to add them to, so the runtime takes them from this class
 * (mg_dvm_class.c, "methods a boot class lacks"): a static method of the same name and signature is MeasureUnit's static
 * one, and one that takes a MeasureUnit first is the method of a MeasureUnit. AndroidX's LocalePreferences asks
 * getIdentifier() of the unit a temperature is shown in (Yandex Maps, Google Play services).
 *
 * Its static fields are MeasureUnit's too, for an app's own code (mg_dvm_class.c, "static fields a boot class lacks"):
 * the units Android 11 to 15 added, in the order they came. ICU 60 can make a unit of any name; megler.NewUnits gives
 * its MeasureFormat the words to write these with. Breezy Weather reads MeasureUnit.PERCENT for a humidity.
 */
package megler.members.android.icu.util;

public final class MeasureUnit {
    public static final android.icu.util.MeasureUnit PERCENT = megler.NewUnits.unit("concentr", "percent");
    public static final android.icu.util.MeasureUnit PERMILLE = megler.NewUnits.unit("concentr", "permille");
    public static final android.icu.util.MeasureUnit ATMOSPHERE = megler.NewUnits.unit("pressure", "atmosphere");
    public static final android.icu.util.MeasureUnit PETABYTE = megler.NewUnits.unit("digital", "petabyte");
    public static final android.icu.util.MeasureUnit BAR = megler.NewUnits.unit("pressure", "bar");
    public static final android.icu.util.MeasureUnit PASCAL = megler.NewUnits.unit("pressure", "pascal");
    public static final android.icu.util.MeasureUnit KILOPASCAL = megler.NewUnits.unit("pressure", "kilopascal");
    public static final android.icu.util.MeasureUnit MEGAPASCAL = megler.NewUnits.unit("pressure", "megapascal");
    public static final android.icu.util.MeasureUnit BARREL = megler.NewUnits.unit("volume", "barrel");
    public static final android.icu.util.MeasureUnit BRITISH_THERMAL_UNIT = megler.NewUnits.unit("energy", "british-thermal-unit");
    public static final android.icu.util.MeasureUnit ELECTRONVOLT = megler.NewUnits.unit("energy", "electronvolt");
    public static final android.icu.util.MeasureUnit THERM_US = megler.NewUnits.unit("energy", "therm-us");
    public static final android.icu.util.MeasureUnit NEWTON = megler.NewUnits.unit("force", "newton");
    public static final android.icu.util.MeasureUnit POUND_FORCE = megler.NewUnits.unit("force", "pound-force");
    public static final android.icu.util.MeasureUnit NEWTON_METER = megler.NewUnits.unit("torque", "newton-meter");
    public static final android.icu.util.MeasureUnit POUND_FOOT = megler.NewUnits.unit("torque", "pound-force-foot");
    public static final android.icu.util.MeasureUnit DALTON = megler.NewUnits.unit("mass", "dalton");
    public static final android.icu.util.MeasureUnit EARTH_MASS = megler.NewUnits.unit("mass", "earth-mass");
    public static final android.icu.util.MeasureUnit SOLAR_MASS = megler.NewUnits.unit("mass", "solar-mass");
    public static final android.icu.util.MeasureUnit SOLAR_LUMINOSITY = megler.NewUnits.unit("light", "solar-luminosity");
    public static final android.icu.util.MeasureUnit SOLAR_RADIUS = megler.NewUnits.unit("length", "solar-radius");
    public static final android.icu.util.MeasureUnit MOLE = megler.NewUnits.unit("concentr", "mole");
    public static final android.icu.util.MeasureUnit PERMYRIAD = megler.NewUnits.unit("concentr", "permyriad");
    public static final android.icu.util.MeasureUnit DUNAM = megler.NewUnits.unit("area", "dunam");
    public static final android.icu.util.MeasureUnit FLUID_OUNCE_IMPERIAL = megler.NewUnits.unit("volume", "fluid-ounce-imperial");
    public static final android.icu.util.MeasureUnit DAY_PERSON = megler.NewUnits.unit("duration", "day-person");
    public static final android.icu.util.MeasureUnit WEEK_PERSON = megler.NewUnits.unit("duration", "week-person");
    public static final android.icu.util.MeasureUnit MONTH_PERSON = megler.NewUnits.unit("duration", "month-person");
    public static final android.icu.util.MeasureUnit YEAR_PERSON = megler.NewUnits.unit("duration", "year-person");
    public static final android.icu.util.MeasureUnit DECADE = megler.NewUnits.unit("duration", "decade");
    public static final android.icu.util.MeasureUnit DOT_PER_CENTIMETER = megler.NewUnits.unit("graphics", "dot-per-centimeter");
    public static final android.icu.util.MeasureUnit DOT_PER_INCH = megler.NewUnits.unit("graphics", "dot-per-inch");
    public static final android.icu.util.MeasureUnit EM = megler.NewUnits.unit("graphics", "em");
    public static final android.icu.util.MeasureUnit MEGAPIXEL = megler.NewUnits.unit("graphics", "megapixel");
    public static final android.icu.util.MeasureUnit PIXEL = megler.NewUnits.unit("graphics", "pixel");
    public static final android.icu.util.MeasureUnit PIXEL_PER_CENTIMETER = megler.NewUnits.unit("graphics", "pixel-per-centimeter");
    public static final android.icu.util.MeasureUnit PIXEL_PER_INCH = megler.NewUnits.unit("graphics", "pixel-per-inch");
    public static final android.icu.util.MeasureUnit CANDELA = megler.NewUnits.unit("light", "candela");
    public static final android.icu.util.MeasureUnit LUMEN = megler.NewUnits.unit("light", "lumen");
    public static final android.icu.util.MeasureUnit DESSERT_SPOON = megler.NewUnits.unit("volume", "dessert-spoon");
    public static final android.icu.util.MeasureUnit DESSERT_SPOON_IMPERIAL = megler.NewUnits.unit("volume", "dessert-spoon-imperial");
    public static final android.icu.util.MeasureUnit DOT = megler.NewUnits.unit("graphics", "dot");
    public static final android.icu.util.MeasureUnit DRAM = megler.NewUnits.unit("volume", "dram");
    public static final android.icu.util.MeasureUnit DROP = megler.NewUnits.unit("volume", "drop");
    public static final android.icu.util.MeasureUnit EARTH_RADIUS = megler.NewUnits.unit("length", "earth-radius");
    public static final android.icu.util.MeasureUnit GRAIN = megler.NewUnits.unit("mass", "grain");
    public static final android.icu.util.MeasureUnit JIGGER = megler.NewUnits.unit("volume", "jigger");
    public static final android.icu.util.MeasureUnit PINCH = megler.NewUnits.unit("volume", "pinch");
    public static final android.icu.util.MeasureUnit QUART_IMPERIAL = megler.NewUnits.unit("volume", "quart-imperial");
    public static final android.icu.util.MeasureUnit MILLIGRAM_OFGLUCOSE_PER_DECILITER = megler.NewUnits.unit("concentr", "milligram-ofglucose-per-deciliter");
    public static final android.icu.util.MeasureUnit ITEM = megler.NewUnits.unit("concentr", "item");
    public static final android.icu.util.MeasureUnit KILOWATT_HOUR_PER_100_KILOMETER = megler.NewUnits.unit("consumption", "kilowatt-hour-per-100-kilometer");
    public static final android.icu.util.MeasureUnit QUARTER = megler.NewUnits.unit("duration", "quarter");
    public static final android.icu.util.MeasureUnit TONNE = megler.NewUnits.unit("mass", "tonne");

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

    /*
     * ICU 60 has a percent and a permille that are no units (NoUnit.PERCENT, of the type "none"), which the later ICUs
     * dropped for MeasureUnit.PERCENT and PERMILLE: a unit of a real type is the one an identifier names.
     */
    public static android.icu.util.MeasureUnit forIdentifier(String identifier) {
        if (identifier == null || identifier.isEmpty()) return android.icu.util.NoUnit.BASE;
        android.icu.util.MeasureUnit none = null;
        for (android.icu.util.MeasureUnit u : android.icu.util.MeasureUnit.getAvailable()) {
            if ("currency".equals(u.getType()) || !identifier.equals(getIdentifier(u))) continue;
            if (!"none".equals(u.getType())) return u;
            none = u;
        }
        if (none != null) return none;
        throw new IllegalArgumentException("Unknown unit identifier: " + identifier);
    }
}
