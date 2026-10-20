// Megler: Android 12's system colours (android.R.color.system_*: Material You's palettes, and Android 14's colour roles
// made of them), which the platform's resources here, Android 9's framework-res, do not have.
//
// The framework says it is Android 14, and an app takes it at its word: Compose's Material 3 and Material Components ask
// for android.R.color.system_primary_light and its like on Android 12 and later, by number. Android 9 has other colours
// of its own under most of those numbers (0x01060064 is the selector control_checkable_material) and nothing under some
// (0x01060091, system_tertiary_container_dark, is a colour of a watch's there): Aurora Store's first screen ended on
// "Resource ID #0x1060091" not found. Resources gives these colours in their stead when the app's code asks for one
// (android.content.res.Resources: getColor, getColorStateList), and whenever Android 9 has nothing under the number.
//
// The palettes are made as Android makes them from a seed colour, roughly: each a hue and a chroma, its shades the tones
// of lightness from 100 (shade 0) to 0 (shade 1000); here in CIE LCh (Material uses its own colour space, HCT, whose
// tones are the same L*), the chroma lowered where a tone has no colour that strong. The seed is Android's own when the
// wallpaper gives none, Google's blue; the roles are Material 3's tones of the palettes.
package megler;

public final class SystemColors {
    /** The numbers of Android 12 to 15's system colours. */
    public static final int FIRST = 0x0106001d, LAST = 0x010600d1;

    /** Android's seed when the wallpaper gives no colour. */
    private static final int SEED = 0xFF1B6EF3;

    /** The palettes' shades (0, 10, 50, 100 ... 1000) as tones. */
    private static final int[] SHADE_TONES = {100, 99, 95, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0};

    private static final int N1 = 0, N2 = 1, A1 = 2, A2 = 3, A3 = 4, E = 5;

    /** The roles, 0x0106005e on, in their order: a palette and a tone each (-1: a colour of its own, below). */
    private static final int[][] ROLES = {
        // light: the containers and the colours on them, primary to tertiary
        {A1, 90}, {A1, 10}, {A1, 40}, {A1, 100}, {A2, 90}, {A2, 10}, {A2, 40}, {A2, 100}, {A3, 90}, {A3, 10}, {A3, 40}, {A3, 100},
        // background, surfaces
        {N1, 98}, {N1, 10}, {N1, 98}, {N1, 10}, {N1, 96}, {N1, 100}, {N1, 94}, {N1, 92}, {N1, 90}, {N1, 98}, {N1, 87}, {N2, 90}, {N2, 30}, {N2, 50},
        // error
        {E, 40}, {E, 100}, {E, 90}, {E, 10},
        // controls: activated, normal, highlight
        {A1, 90}, {N2, 30}, {-1, 0x1F000000},
        // text on the inverse
        {N1, 90}, {N2, 80}, {N1, 90}, {N1, 90}, {N1, 90},
        // the palettes' key colours
        {A1, 40}, {A2, 40}, {A3, 40}, {N1, 40}, {N2, 40},
        // dark, as light
        {A1, 30}, {A1, 90}, {A1, 80}, {A1, 20}, {A2, 30}, {A2, 90}, {A2, 80}, {A2, 20}, {A3, 30}, {A3, 90}, {A3, 80}, {A3, 20},
        {N1, 6}, {N1, 90}, {N1, 6}, {N1, 90}, {N1, 10}, {N1, 4}, {N1, 12}, {N1, 17}, {N1, 22}, {N1, 24}, {N1, 6}, {N2, 30}, {N2, 80}, {N2, 60},
        {E, 80}, {E, 20}, {E, 30}, {E, 90},
        {A1, 30}, {N2, 80}, {-1, 0x33FFFFFF},
        {N1, 10}, {N2, 30}, {N1, 10}, {N1, 10}, {N1, 10},
        {A1, 40}, {A2, 40}, {A3, 40}, {N1, 40}, {N2, 40},
        // fixed: primary, its dim, on it, on it a variant; then secondary, tertiary
        {A1, 90}, {A1, 80}, {A1, 10}, {A1, 30}, {A2, 90}, {A2, 80}, {A2, 10}, {A2, 30}, {A3, 90}, {A3, 80}, {A3, 10}, {A3, 30},
        // outline variant, light and dark; surface, on it and outline disabled
        {N2, 80}, {N2, 30}, {-2, 0x665a}, {-2, 0x610a}, {-3, 0x1F32},
    };

    private static int[] sColors;

    private SystemColors() {}

    public static boolean has(int id) { return id >= FIRST && id <= LAST; }

    /** The colour of a system colour's number, ARGB. */
    public static synchronized int color(int id) {
        if (sColors == null) sColors = make(SEED);
        return has(id) ? sColors[id - FIRST] : 0;
    }

    private static int[] make(int seed) {
        double[] lch = lch(seed);
        double hue = lch[2];
        // Each palette's hue and chroma, as Android 12's "tonal spot" makes them (its chromas in HCT, 36, 16, 24, 4 and 8,
        // a little more in L*a*b*'s units), and the error colour's, a red.
        double[][] palettes = {
            {hue, 4}, {hue, 8}, {hue, Math.min(lch[1], 48)}, {hue, 16}, {(hue + 60) % 360, 28}, {35, 70},
        };
        int[] out = new int[LAST - FIRST + 1];
        int at = 0;
        for (int p = 0; p < 5; p++)
            for (int tone : SHADE_TONES) out[at++] = tone(palettes[p], tone);
        for (int[] role : ROLES) {
            if (role[0] >= 0) out[at++] = tone(palettes[role[0]], role[1]);
            else if (role[0] == -1) out[at++] = role[1];
            else {
                // A tone of neutral 1 (-2) or 2 (-3) at an alpha: (alpha << 8) | tone.
                int alpha = role[1] >>> 8, tone = role[1] & 0xFF;
                out[at++] = (alpha << 24) | (tone(palettes[role[0] == -2 ? N1 : N2], tone) & 0xFFFFFF);
            }
        }
        for (int tone : SHADE_TONES) out[at++] = tone(palettes[E], tone);
        return out;
    }

    /** A palette's colour at a tone: L* of the tone, the palette's hue, its chroma or as much of it as the tone allows. */
    private static int tone(double[] palette, int tone) {
        if (tone >= 100) return 0xFFFFFFFF;
        if (tone <= 0) return 0xFF000000;
        double lo = 0, hi = palette[1];
        int best = rgb(tone, 0, 0);
        if (inGamut(tone, hi, palette[0])) return rgb(tone, hi, palette[0]);
        for (int i = 0; i < 20; i++) {
            double mid = (lo + hi) / 2;
            if (inGamut(tone, mid, palette[0])) { lo = mid; best = rgb(tone, mid, palette[0]); } else hi = mid;
        }
        return best;
    }

    private static boolean inGamut(double l, double c, double h) {
        double[] lin = linear(l, c, h);
        for (double v : lin) if (v < -0.0001 || v > 1.0001) return false;
        return true;
    }

    private static int rgb(double l, double c, double h) {
        double[] lin = linear(l, c, h);
        int argb = 0xFF000000;
        for (int k = 0; k < 3; k++) {
            double v = Math.max(0, Math.min(1, lin[k]));
            v = v <= 0.0031308 ? 12.92 * v : 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
            argb |= ((int) Math.round(v * 255)) << (16 - 8 * k);
        }
        return argb;
    }

    /** CIE LCh (D65) to linear sRGB. */
    private static double[] linear(double l, double c, double h) {
        double a = c * Math.cos(Math.toRadians(h)), b = c * Math.sin(Math.toRadians(h));
        double fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
        double x = 0.95047 * finv(fx), y = finv(fy), z = 1.08883 * finv(fz);
        return new double[] {
            3.2404542 * x - 1.5371385 * y - 0.4985314 * z,
            -0.9692660 * x + 1.8760108 * y + 0.0415560 * z,
            0.0556434 * x - 0.2040259 * y + 1.0572252 * z,
        };
    }

    private static double finv(double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); }

    /** sRGB to CIE LCh (D65): L*, C, h in degrees. */
    private static double[] lch(int argb) {
        double[] lin = new double[3];
        for (int k = 0; k < 3; k++) {
            double v = ((argb >> (16 - 8 * k)) & 0xFF) / 255.0;
            lin[k] = v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
        }
        double x = (0.4124564 * lin[0] + 0.3575761 * lin[1] + 0.1804375 * lin[2]) / 0.95047;
        double y = 0.2126729 * lin[0] + 0.7151522 * lin[1] + 0.0721750 * lin[2];
        double z = (0.0193339 * lin[0] + 0.1191920 * lin[1] + 0.9503041 * lin[2]) / 1.08883;
        double fx = f(x), fy = f(y), fz = f(z);
        double l = 116 * fy - 16, a = 500 * (fx - fy), b = 200 * (fy - fz);
        double h = Math.toDegrees(Math.atan2(b, a));
        return new double[] {l, Math.hypot(a, b), h < 0 ? h + 360 : h};
    }

    private static double f(double t) { return t > 216.0 / 24389 ? Math.cbrt(t) : (24389.0 / 27 * t + 16) / 116; }
}
