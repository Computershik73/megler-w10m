/*
 * Material You's tonal palettes made from a seed colour (megler.h, megler_palette_tone): what Android 12 makes of a
 * wallpaper's colour for its system colours, roughly, which a Java app's framework gives for android.R.color.system_*
 * (megler.SystemColors), and of which the shell makes its own Material look. Here the seed is the phone's accent colour.
 *
 * Each palette is a hue and a chroma, its colours the tones of lightness from 0 (black) to 100 (white): here in CIE LCh
 * (D65), a tone being L* (Material's own colour space, HCT, has the same tones), the chroma lowered where a tone has no
 * colour that strong in sRGB. The six palettes are those of Android 12's "tonal spot": the two neutrals of the seed's hue
 * and of a chroma of 4 and 8, the first accent of the seed's own chroma up to 48, the second of 16, the third a sixth of
 * the way round the hue circle at 28, and the error colour, a red.
 */
#include "../mg_base.h"

#include <math.h>

#include "../../include/megler.h"

static const double k_pi = 3.14159265358979323846;

static double finv(double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); }
static double fwd(double t) { return t > 216.0 / 24389 ? cbrt(t) : (24389.0 / 27 * t + 16) / 116; }

/* CIE LCh (D65) to linear sRGB. */
static void linear(double l, double c, double h, double out[3])
{
    double a = c * cos(h * k_pi / 180), b = c * sin(h * k_pi / 180);
    double fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    double x = 0.95047 * finv(fx), y = finv(fy), z = 1.08883 * finv(fz);
    out[0] = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    out[1] = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    out[2] = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
}

static bool in_gamut(double l, double c, double h)
{
    double lin[3];
    int k;
    linear(l, c, h, lin);
    for (k = 0; k < 3; k++)
        if (lin[k] < -0.0001 || lin[k] > 1.0001) return false;
    return true;
}

static uint32_t rgb_of(double l, double c, double h)
{
    double lin[3];
    uint32_t argb = 0xFF000000u;
    int k;
    linear(l, c, h, lin);
    for (k = 0; k < 3; k++) {
        double v = lin[k] < 0 ? 0 : lin[k] > 1 ? 1 : lin[k];
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
        argb |= (uint32_t)lround(v * 255) << (16 - 8 * k);
    }
    return argb;
}

/* sRGB to CIE LCh (D65): L*, C, h in degrees. */
static void lch_of(uint32_t argb, double out[3])
{
    double lin[3], x, y, z, fx, fy, fz, a, b, h;
    int k;
    for (k = 0; k < 3; k++) {
        double v = ((argb >> (16 - 8 * k)) & 0xFF) / 255.0;
        lin[k] = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    x = (0.4124564 * lin[0] + 0.3575761 * lin[1] + 0.1804375 * lin[2]) / 0.95047;
    y = 0.2126729 * lin[0] + 0.7151522 * lin[1] + 0.0721750 * lin[2];
    z = (0.0193339 * lin[0] + 0.1191920 * lin[1] + 0.9503041 * lin[2]) / 1.08883;
    fx = fwd(x), fy = fwd(y), fz = fwd(z);
    a = 500 * (fx - fy), b = 200 * (fy - fz);
    h = atan2(b, a) * 180 / k_pi;
    out[0] = 116 * fy - 16;
    out[1] = hypot(a, b);
    out[2] = h < 0 ? h + 360 : h;
}

/* A palette's colour at a tone: L* of the tone, the palette's hue, its chroma or as much of it as the tone allows. */
static uint32_t tone_of(double hue, double chroma, int tone)
{
    double lo = 0, hi = chroma;
    uint32_t best;
    int i;
    if (tone >= 100) return 0xFFFFFFFFu;
    if (tone <= 0) return 0xFF000000u;
    if (in_gamut(tone, hi, hue)) return rgb_of(tone, hi, hue);
    best = rgb_of(tone, 0, 0);
    for (i = 0; i < 20; i++) {
        double mid = (lo + hi) / 2;
        if (in_gamut(tone, mid, hue)) {
            lo = mid;
            best = rgb_of(tone, mid, hue);
        } else {
            hi = mid;
        }
    }
    return best;
}

uint32_t megler_palette_tone(uint32_t seed_argb, int palette, int tone)
{
    double lch[3], hue, chroma;
    lch_of(seed_argb, lch);
    hue = lch[2];
    switch (palette) {
    case MEGLER_PALETTE_NEUTRAL1: chroma = 4; break;
    case MEGLER_PALETTE_NEUTRAL2: chroma = 8; break;
    case MEGLER_PALETTE_ACCENT1: chroma = lch[1] < 48 ? lch[1] : 48; break;
    case MEGLER_PALETTE_ACCENT2: chroma = 16; break;
    case MEGLER_PALETTE_ACCENT3: hue = fmod(hue + 60, 360), chroma = 28; break;
    case MEGLER_PALETTE_ERROR: hue = 35, chroma = 70; break;
    default: return 0;
    }
    return tone_of(hue, chroma, tone);
}
