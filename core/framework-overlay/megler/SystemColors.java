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
// The palettes are made as Android makes them from a seed colour, roughly (the core's megler_palette_tone, which the
// shell's own Material look is made with too: core/src/run/mg_palette.c); the roles are Material 3's tones of the
// palettes. The seed is the phone's accent colour (megler_set_theme, Native.accentColor), as Android takes the
// wallpaper's; with none told, Android's own when the wallpaper gives none, Google's blue.
package megler;

public final class SystemColors {
    /** The numbers of Android 12 to 15's system colours. */
    public static final int FIRST = 0x0106001d, LAST = 0x010600d1;

    /** Android's seed when the wallpaper gives no colour. */
    private static final int SEED = 0xFF1B6EF3;

    /** The palettes' shades (0, 10, 50, 100 ... 1000) as tones. */
    private static final int[] SHADE_TONES = {100, 99, 95, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0};

    /** The palettes, as megler_palette_tone numbers them. */
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
        if (sColors == null) {
            int accent = Native.accentColor();
            sColors = make(accent != 0 ? accent | 0xFF000000 : SEED);
        }
        return has(id) ? sColors[id - FIRST] : 0;
    }

    private static int[] make(int seed) {
        int[] out = new int[LAST - FIRST + 1];
        int at = 0;
        for (int p = 0; p < 5; p++)
            for (int tone : SHADE_TONES) out[at++] = Native.paletteTone(seed, p, tone);
        for (int[] role : ROLES) {
            if (role[0] >= 0) out[at++] = Native.paletteTone(seed, role[0], role[1]);
            else if (role[0] == -1) out[at++] = role[1];
            else {
                // A tone of neutral 1 (-2) or 2 (-3) at an alpha: (alpha << 8) | tone.
                int alpha = role[1] >>> 8, tone = role[1] & 0xFF;
                out[at++] = (alpha << 24) | (Native.paletteTone(seed, role[0] == -2 ? N1 : N2, tone) & 0xFFFFFF);
            }
        }
        for (int tone : SHADE_TONES) out[at++] = Native.paletteTone(seed, E, tone);
        return out;
    }
}
