/*
 * Megler: the unit a formatter's usage (NumberFormatterSettings.usage, Android 12's) shows a quantity in, in the
 * formatter's locale, and the quantity converted to it. ICU takes it from CLDR's unit preferences, which Android 9's ICU
 * 60 does not have; these are those of a few kinds of quantity -- temperature, length, mass, speed, volume, area, fuel
 * consumption, air pressure -- for the regions CLDR sets apart, each list of units as CLDR gives it, the first unit with at
 * least its amount the one used (the last for any amount). A locale's "mu" says the unit of temperature, its "ms" the
 * measurement system, its "rg" the region, as for ICU. CLDR's mixed units (a person's height in feet and inches) are the
 * larger unit alone here. NumberFormatter.java says why this package is here.
 */
package android.icu.number;

import android.icu.util.MeasureUnit;
import android.icu.util.ULocale;
import java.math.BigDecimal;
import java.math.MathContext;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

final class Units {
    static final class Preferred {
        final MeasureUnit unit;
        final BigDecimal value;

        Preferred(MeasureUnit unit, BigDecimal value) {
            this.unit = unit;
            this.value = value;
        }
    }

    private static final MathContext MC = MathContext.DECIMAL64;

    /* Each unit's size in its kind's base unit: meter, kilogram, meter-per-second, cubic-meter, square-meter, hectopascal, liter-per-kilometer. */
    private static final Map<String, BigDecimal> FACTORS = new HashMap<>();
    /* Fuel consumption the other way round: distance per volume, so that liter-per-kilometer is this over the value. */
    private static final Map<String, BigDecimal> INVERSE = new HashMap<>();
    private static final Map<String, MeasureUnit> FOUND = new HashMap<>();

    private static final String[][] PREFERENCES = {
        { "temperature", "default", "001", "celsius" },
        { "temperature", "default", "BS BZ KY PR PW US", "fahrenheit" },
        { "temperature", "weather", "001", "celsius" },
        { "temperature", "weather", "BS BZ KY PR PW US", "fahrenheit" },
        { "length", "default", "001", "kilometer:1 meter:1 centimeter" },
        { "length", "default", "US", "mile:1 foot:1 inch" },
        { "length", "road", "001", "kilometer:0.9 meter" },
        { "length", "road", "US", "mile:0.5 foot" },
        { "length", "road", "GB", "mile:0.5 yard" },
        { "length", "person-height", "001", "centimeter" },
        { "length", "person-height", "CA GB IN US", "foot" },
        { "length", "rainfall", "001", "millimeter" },
        { "length", "rainfall", "US", "inch" },
        { "length", "snowfall", "001", "centimeter" },
        { "length", "snowfall", "US", "inch" },
        { "length", "visiblty", "001", "kilometer:1 meter" },
        { "length", "visiblty", "US", "mile:1 foot" },
        { "mass", "default", "001", "kilogram:1 gram" },
        { "mass", "default", "GB US", "pound:1 ounce" },
        { "mass", "person", "001", "kilogram" },
        { "mass", "person", "US", "pound" },
        { "mass", "person", "GB IE", "stone" },
        { "speed", "default", "001", "kilometer-per-hour" },
        { "speed", "default", "GB US", "mile-per-hour" },
        { "speed", "wind", "001", "kilometer-per-hour" },
        { "speed", "wind", "GB US", "mile-per-hour" },
        { "volume", "default", "001", "cubic-meter:1 cubic-centimeter" },
        { "volume", "default", "US", "cubic-foot:1 cubic-inch" },
        { "volume", "fluid", "001", "liter:1 milliliter" },
        { "volume", "fluid", "US", "gallon:1 quart:1 pint:1 cup:1 fluid-ounce" },
        { "volume", "vehicle-fuel", "001", "liter" },
        { "volume", "vehicle-fuel", "US", "gallon" },
        { "area", "default", "001", "square-kilometer:1 hectare:1 square-meter:1 square-centimeter" },
        { "area", "default", "GB US", "square-mile:1 acre:1 square-foot:1 square-inch" },
        { "area", "land", "001", "hectare" },
        { "area", "land", "GB US", "acre" },
        { "consumption", "vehicle-fuel", "001", "liter-per-100kilometers" },
        { "consumption", "vehicle-fuel", "US", "mile-per-gallon" },
        { "consumption", "vehicle-fuel", "GB", "mile-per-gallon-imperial" },
        { "pressure", "baromtrc", "001", "hectopascal" },
        { "pressure", "baromtrc", "US", "inch-hg" },
    };

    private static void factor(String subtype, String size) { FACTORS.put(subtype, new BigDecimal(size)); }

    static {
        factor("meter", "1"); factor("kilometer", "1000"); factor("decimeter", "0.1"); factor("centimeter", "0.01"); factor("millimeter", "0.001");
        factor("micrometer", "1E-6"); factor("nanometer", "1E-9"); factor("mile", "1609.344"); factor("yard", "0.9144"); factor("foot", "0.3048");
        factor("inch", "0.0254"); factor("nautical-mile", "1852"); factor("mile-scandinavian", "10000");
        factor("kilogram", "1"); factor("gram", "0.001"); factor("milligram", "1E-6"); factor("microgram", "1E-9"); factor("metric-ton", "1000");
        factor("pound", "0.45359237"); factor("ounce", "0.028349523125"); factor("stone", "6.35029318"); factor("ton", "907.18474");
        factor("meter-per-second", "1"); factor("kilometer-per-hour", "0.27777777777777778"); factor("mile-per-hour", "0.44704");
        factor("knot", "0.51444444444444444");
        factor("cubic-meter", "1"); factor("cubic-centimeter", "1E-6"); factor("cubic-kilometer", "1E9"); factor("cubic-foot", "0.028316846592");
        factor("cubic-inch", "0.000016387064"); factor("cubic-yard", "0.764554857984"); factor("liter", "0.001"); factor("milliliter", "1E-6");
        factor("centiliter", "1E-5"); factor("deciliter", "1E-4"); factor("hectoliter", "0.1"); factor("gallon", "0.003785411784");
        factor("gallon-imperial", "0.00454609"); factor("quart", "0.000946352946"); factor("pint", "0.000473176473"); factor("cup", "0.0002365882365");
        factor("fluid-ounce", "0.0000295735295625"); factor("tablespoon", "0.0000147867647813"); factor("teaspoon", "0.00000492892159375");
        factor("square-meter", "1"); factor("square-kilometer", "1E6"); factor("hectare", "1E4"); factor("square-centimeter", "1E-4");
        factor("square-mile", "2589988.110336"); factor("acre", "4046.8564224"); factor("square-yard", "0.83612736"); factor("square-foot", "0.09290304");
        factor("square-inch", "0.00064516");
        factor("hectopascal", "1"); factor("millibar", "1"); factor("inch-hg", "33.86389"); factor("millimeter-of-mercury", "1.33322387415");
        factor("pound-per-square-inch", "68.94757293168");
        factor("liter-per-kilometer", "1"); factor("liter-per-100kilometers", "0.01");
        INVERSE.put("mile-per-gallon", new BigDecimal("2.352145833"));
        INVERSE.put("mile-per-gallon-imperial", new BigDecimal("2.824809363"));
    }

    private Units() {}

    /* The unit and value for this usage in this locale; null when the unit is not of a kind that has preferences here. */
    static Preferred preferred(MeasureUnit unit, String usage, ULocale locale, BigDecimal value) {
        String category = unit.getType(), subtype = unit.getSubtype();
        String region = region(locale);
        String units = null;
        if ("temperature".equals(category)) {
            String mu = keyword(locale, "mu");
            if ("celsius".equals(mu) || "kelvin".equals(mu)) units = mu;
            else if ("fahrenhe".equals(mu)) units = "fahrenheit";
        }
        if (units == null) units = preference(category, usage, region);
        if (units == null) units = preference(category, "default", region);
        if (units == null) return null;
        BigDecimal base = toBase(subtype, value);
        if (base == null) return null;
        String[] choices = units.split(" ");
        for (int i = 0; i < choices.length; i++) {
            String[] choice = choices[i].split(":");
            BigDecimal converted = fromBase(choice[0], base);
            if (converted == null) continue;
            if (i == choices.length - 1 || choice.length < 2 || converted.abs().compareTo(new BigDecimal(choice[1])) >= 0) {
                MeasureUnit out = unit(category, choice[0]);
                return out == null ? null : new Preferred(out, converted);
            }
        }
        return null;
    }

    private static String preference(String category, String usage, String region) {
        String world = null;
        for (String[] p : PREFERENCES) {
            if (!p[0].equals(category) || !p[1].equals(usage)) continue;
            if (p[2].equals("001")) world = p[3];
            else if ((" " + p[2] + " ").contains(" " + region + " ")) return p[3];
        }
        return world;
    }

    private static String keyword(ULocale locale, String key) {
        try {
            String v = locale.getUnicodeLocaleType(key);
            return v == null ? null : v.toLowerCase(Locale.ROOT);
        } catch (RuntimeException e) {
            return null;
        }
    }

    private static String region(ULocale locale) {
        String rg = keyword(locale, "rg");
        if (rg != null && rg.length() >= 2 && Character.isLetter(rg.charAt(0)) && Character.isLetter(rg.charAt(1))) return rg.substring(0, 2).toUpperCase(Locale.ROOT);
        String ms = keyword(locale, "ms");
        if ("metric".equals(ms)) return "001";
        if ("ussystem".equals(ms)) return "US";
        if ("uksystem".equals(ms)) return "GB";
        String country = locale.getCountry();
        if (country == null || country.isEmpty()) {
            ULocale full = ULocale.addLikelySubtags(locale);
            country = full != null ? full.getCountry() : null;
        }
        return country == null || country.isEmpty() ? "001" : country;
    }

    private static final BigDecimal ZERO_CELSIUS = new BigDecimal("273.15"), NINE_FIFTHS = new BigDecimal("1.8"), FREEZING_F = new BigDecimal("32");

    private static BigDecimal toBase(String subtype, BigDecimal v) {
        switch (subtype) {
        case "kelvin": return v;
        case "celsius": return v.add(ZERO_CELSIUS);
        case "fahrenheit": return v.subtract(FREEZING_F).divide(NINE_FIFTHS, MC).add(ZERO_CELSIUS);
        default: break;
        }
        BigDecimal inverse = INVERSE.get(subtype);
        if (inverse != null) return v.signum() == 0 ? null : inverse.divide(v, MC);
        BigDecimal f = FACTORS.get(subtype);
        return f == null ? null : v.multiply(f, MC);
    }

    private static BigDecimal fromBase(String subtype, BigDecimal base) {
        switch (subtype) {
        case "kelvin": return base;
        case "celsius": return base.subtract(ZERO_CELSIUS);
        case "fahrenheit": return base.subtract(ZERO_CELSIUS).multiply(NINE_FIFTHS).add(FREEZING_F);
        default: break;
        }
        BigDecimal inverse = INVERSE.get(subtype);
        if (inverse != null) return base.signum() == 0 ? null : inverse.divide(base, MC);
        BigDecimal f = FACTORS.get(subtype);
        return f == null ? null : base.divide(f, MC);
    }

    /* ICU 60's unit of this kind and name. */
    private static MeasureUnit unit(String category, String subtype) {
        String key = category + "/" + subtype;
        synchronized (FOUND) {
            if (FOUND.containsKey(key)) return FOUND.get(key);
            MeasureUnit found = null;
            for (MeasureUnit u : MeasureUnit.getAvailable(category))
                if (subtype.equals(u.getSubtype())) found = u;
            FOUND.put(key, found);
            return found;
        }
    }
}
