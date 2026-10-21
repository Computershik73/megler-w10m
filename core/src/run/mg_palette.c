/*
 * Material You's tonal palettes made from a seed colour (megler.h, megler_palette_tone): what Android 12 makes of a
 * wallpaper's colour for its system colours, which a Java app's framework gives for android.R.color.system_*
 * (megler.SystemColors), and of which the shell makes its own Material look. Here the seed is the phone's accent colour.
 *
 * Material's colour space, HCT: the hue and chroma of CAM16 (CIE's colour appearance model of 2016, in Material's default
 * viewing conditions) and the tone of CIE L*. A palette is a hue and a chroma; its colour at a tone is the colour of that
 * hue and L* with that chroma, or the most chroma sRGB has there. The palettes are Material's CorePalette (2021): the first
 * accent the seed's hue at its chroma but at least 48, the second at 16, the third a sixth of the way round at 24, the two
 * neutrals at 4 and 8, the error colour a red (hue 25, chroma 84). The solver is the one Material You came with: for the
 * chroma wanted, then less and less of it, a CAM16 lightness J found by halving whose colour, clipped to sRGB, has the
 * tone and stays near the hue. (CIE LCh, which this file used before, turns a blue seed violet.) Ported from Google's
 * material-color-utilities (Apache License 2.0).
 */
#include "../mg_base.h"

#include <math.h>

#include "../../include/megler.h"

static const double k_pi = 3.14159265358979323846;

/* ------------------------------------------------------------- sRGB, XYZ, L* */

static double y_from_lstar(double lstar)
{
    double ft = (lstar + 16.0) / 116.0, ft3 = ft * ft * ft;
    return 100.0 * (ft3 > 216.0 / 24389.0 ? ft3 : (116.0 * ft - 16.0) / (24389.0 / 27.0));
}

static double lstar_from_y(double y)
{
    double yn = y / 100.0;
    return yn > 216.0 / 24389.0 ? 116.0 * cbrt(yn) - 16.0 : 24389.0 / 27.0 * yn;
}

/* A channel of 0 to 255, linear, 0 to 100. */
static double linearized(int c)
{
    double v = c / 255.0;
    return 100.0 * (v <= 0.040449936 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
}

/* Linear, 0 to 100, as a channel of 0 to 255. */
static int delinearized(double l)
{
    double n = l / 100.0, v = n <= 0.0031308 ? n * 12.92 : 1.055 * pow(n, 1.0 / 2.4) - 0.055;
    long r = lround(v * 255.0);
    return r < 0 ? 0 : r > 255 ? 255 : (int)r;
}

static void xyz_from_argb(uint32_t argb, double xyz[3])
{
    double r = linearized((argb >> 16) & 0xFF), g = linearized((argb >> 8) & 0xFF), b = linearized(argb & 0xFF);
    xyz[0] = 0.41233895 * r + 0.35762064 * g + 0.18051042 * b;
    xyz[1] = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    xyz[2] = 0.01932141 * r + 0.11916382 * g + 0.95034478 * b;
}

static uint32_t argb_from_xyz(const double xyz[3])
{
    double r = 3.2413774792388685 * xyz[0] - 1.5376652402851851 * xyz[1] - 0.49885366846268053 * xyz[2];
    double g = -0.9691452513005321 * xyz[0] + 1.8758853451067872 * xyz[1] + 0.04156585616912061 * xyz[2];
    double b = 0.05562093689691305 * xyz[0] - 0.20395524564742123 * xyz[1] + 1.0571799111220335 * xyz[2];
    return 0xFF000000u | ((uint32_t)delinearized(r) << 16) | ((uint32_t)delinearized(g) << 8) | (uint32_t)delinearized(b);
}

static double lstar_from_argb(uint32_t argb)
{
    double xyz[3];
    xyz_from_argb(argb, xyz);
    return lstar_from_y(xyz[1]);
}

static uint32_t argb_from_lstar(double lstar)
{
    int c = delinearized(y_from_lstar(lstar));
    return 0xFF000000u | ((uint32_t)c << 16) | ((uint32_t)c << 8) | (uint32_t)c;
}

/* ------------------------------------------------------------- CAM16 */

/* Material's default viewing conditions: D65, an adapting luminance of an L* of 50's, a background of L* 50, an average surround. */
static struct {
    bool made;
    double n, aw, nbb, ncb, c, nc, fl, fl_root, z, rgb_d[3];
} vc;

static void make_viewing_conditions(void)
{
    static const double white[3] = { 95.047, 100.0, 108.883 };
    double la = 200.0 / k_pi * y_from_lstar(50.0) / 100.0, f = 0.8 + 2.0 / 10.0, d, k, k4, k4f, rw, gw, bw, af[3], a[3];
    int i;
    if (vc.made) return;
    rw = 0.401288 * white[0] + 0.650173 * white[1] - 0.051461 * white[2];
    gw = -0.250268 * white[0] + 1.204414 * white[1] + 0.045854 * white[2];
    bw = -0.002079 * white[0] + 0.048952 * white[1] + 0.953127 * white[2];
    vc.c = f >= 0.9 ? 0.59 + (0.69 - 0.59) * ((f - 0.9) * 10.0) : 0.525 + (0.59 - 0.525) * ((f - 0.8) * 10.0);
    d = f * (1.0 - (1.0 / 3.6) * exp((-la - 42.0) / 92.0));
    d = d < 0 ? 0 : d > 1 ? 1 : d;
    vc.nc = f;
    vc.rgb_d[0] = d * (100.0 / rw) + 1.0 - d;
    vc.rgb_d[1] = d * (100.0 / gw) + 1.0 - d;
    vc.rgb_d[2] = d * (100.0 / bw) + 1.0 - d;
    k = 1.0 / (5.0 * la + 1.0);
    k4 = k * k * k * k;
    k4f = 1.0 - k4;
    vc.fl = k4 * la + 0.1 * k4f * k4f * cbrt(5.0 * la);
    vc.fl_root = pow(vc.fl, 0.25);
    vc.n = y_from_lstar(50.0) / white[1];
    vc.z = 1.48 + sqrt(vc.n);
    vc.nbb = 0.725 / pow(vc.n, 0.2);
    vc.ncb = vc.nbb;
    af[0] = pow(vc.fl * vc.rgb_d[0] * rw / 100.0, 0.42);
    af[1] = pow(vc.fl * vc.rgb_d[1] * gw / 100.0, 0.42);
    af[2] = pow(vc.fl * vc.rgb_d[2] * bw / 100.0, 0.42);
    for (i = 0; i < 3; i++) a[i] = 400.0 * af[i] / (af[i] + 27.13);
    vc.aw = (2.0 * a[0] + a[1] + 0.05 * a[2]) * vc.nbb;
    vc.made = true;
}

typedef struct cam16 {
    double j, chroma, hue, jstar, astar, bstar;
} cam16;

/* The uniform coordinates CAM16-UCS has for a lightness J, a chroma and a hue. */
static void ucs(cam16 *c)
{
    double m = c->chroma * vc.fl_root, mstar = 1.0 / 0.0228 * log1p(0.0228 * m), hr = c->hue * k_pi / 180.0;
    c->jstar = (1.0 + 100.0 * 0.007) * c->j / (1.0 + 0.007 * c->j);
    c->astar = mstar * cos(hr);
    c->bstar = mstar * sin(hr);
}

static cam16 cam16_from_argb(uint32_t argb)
{
    double xyz[3], rc, gc, bc, rd, gd, bd, raf, gaf, baf, ra, ga, ba, a, b, u, p2, h, ac, j, hp, ehue, p1, t, alpha;
    cam16 out;
    xyz_from_argb(argb, xyz);
    rc = 0.401288 * xyz[0] + 0.650173 * xyz[1] - 0.051461 * xyz[2];
    gc = -0.250268 * xyz[0] + 1.204414 * xyz[1] + 0.045854 * xyz[2];
    bc = -0.002079 * xyz[0] + 0.048952 * xyz[1] + 0.953127 * xyz[2];
    rd = vc.rgb_d[0] * rc, gd = vc.rgb_d[1] * gc, bd = vc.rgb_d[2] * bc;
    raf = pow(vc.fl * fabs(rd) / 100.0, 0.42);
    gaf = pow(vc.fl * fabs(gd) / 100.0, 0.42);
    baf = pow(vc.fl * fabs(bd) / 100.0, 0.42);
    ra = (rd < 0 ? -1 : 1) * 400.0 * raf / (raf + 27.13);
    ga = (gd < 0 ? -1 : 1) * 400.0 * gaf / (gaf + 27.13);
    ba = (bd < 0 ? -1 : 1) * 400.0 * baf / (baf + 27.13);
    a = (11.0 * ra - 12.0 * ga + ba) / 11.0;
    b = (ra + ga - 2.0 * ba) / 9.0;
    u = (20.0 * ra + 20.0 * ga + 21.0 * ba) / 20.0;
    p2 = (40.0 * ra + 20.0 * ga + ba) / 20.0;
    h = atan2(b, a) * 180.0 / k_pi;
    h = h < 0 ? h + 360.0 : h >= 360.0 ? h - 360.0 : h;
    ac = p2 * vc.nbb;
    j = 100.0 * pow(ac / vc.aw, vc.c * vc.z);
    hp = h < 20.14 ? h + 360.0 : h;
    ehue = 0.25 * (cos(hp * k_pi / 180.0 + 2.0) + 3.8);
    p1 = 50000.0 / 13.0 * ehue * vc.nc * vc.ncb;
    t = p1 * hypot(a, b) / (u + 0.305);
    alpha = pow(1.64 - pow(0.29, vc.n), 0.73) * pow(t, 0.9);
    out.j = j;
    out.chroma = alpha * sqrt(j / 100.0);
    out.hue = h;
    ucs(&out);
    return out;
}

static cam16 cam16_from_jch(double j, double chroma, double hue)
{
    cam16 out;
    out.j = j;
    out.chroma = chroma;
    out.hue = hue;
    ucs(&out);
    return out;
}

/* The colour of a lightness J, a chroma and a hue, clipped to sRGB. */
static uint32_t argb_from_cam16(const cam16 *c)
{
    double alpha = (c->chroma == 0.0 || c->j == 0.0) ? 0.0 : c->chroma / sqrt(c->j / 100.0);
    double t = pow(alpha / pow(1.64 - pow(0.29, vc.n), 0.73), 1.0 / 0.9);
    double hr = c->hue * k_pi / 180.0, ehue = 0.25 * (cos(hr + 2.0) + 3.8);
    double ac = vc.aw * pow(c->j / 100.0, 1.0 / vc.c / vc.z), p1 = ehue * (50000.0 / 13.0) * vc.nc * vc.ncb, p2 = ac / vc.nbb;
    double hs = sin(hr), hc = cos(hr);
    double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * hc + 108.0 * t * hs);
    double a = gamma * hc, b = gamma * hs;
    double ra = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
    double ga = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
    double ba = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;
    double rcb = fmax(0.0, 27.13 * fabs(ra) / (400.0 - fabs(ra)));
    double gcb = fmax(0.0, 27.13 * fabs(ga) / (400.0 - fabs(ga)));
    double bcb = fmax(0.0, 27.13 * fabs(ba) / (400.0 - fabs(ba)));
    double rc = (ra < 0 ? -1 : 1) * (100.0 / vc.fl) * pow(rcb, 1.0 / 0.42);
    double gc = (ga < 0 ? -1 : 1) * (100.0 / vc.fl) * pow(gcb, 1.0 / 0.42);
    double bc = (ba < 0 ? -1 : 1) * (100.0 / vc.fl) * pow(bcb, 1.0 / 0.42);
    double rf = rc / vc.rgb_d[0], gf = gc / vc.rgb_d[1], bf = bc / vc.rgb_d[2], xyz[3];
    xyz[0] = 1.86206786 * rf - 1.01125463 * gf + 0.14918677 * bf;
    xyz[1] = 0.38752654 * rf + 0.62144744 * gf - 0.00897398 * bf;
    xyz[2] = -0.01584150 * rf - 0.03412294 * gf + 1.04996444 * bf;
    return argb_from_xyz(xyz);
}

static double distance(const cam16 *x, const cam16 *y)
{
    double dj = x->jstar - y->jstar, da = x->astar - y->astar, db = x->bstar - y->bstar;
    return 1.41 * pow(sqrt(dj * dj + da * da + db * db), 0.63);
}

/* ------------------------------------------------------------- HCT */

/* The colour of a hue and a chroma whose tone is `lstar`, found by halving CAM16's J; false when none is near enough. */
static bool find_by_j(double hue, double chroma, double lstar, uint32_t *found)
{
    double low = 0.0, high = 100.0, best_dl = 1000.0, best_de = 1000.0;
    bool any = false;
    while (fabs(low - high) > 0.01) {
        double mid = low + (high - low) / 2.0, clipped_lstar, dl;
        cam16 before = cam16_from_jch(mid, chroma, hue);
        uint32_t clipped = argb_from_cam16(&before);
        clipped_lstar = lstar_from_argb(clipped);
        dl = fabs(lstar - clipped_lstar);
        if (dl < 0.2) {
            cam16 after = cam16_from_argb(clipped), wanted = cam16_from_jch(after.j, after.chroma, hue);
            double de = distance(&after, &wanted);
            if (de <= 1.0 && de <= best_de) {
                best_dl = dl;
                best_de = de;
                *found = clipped;
                any = true;
            }
        }
        if (best_dl == 0.0 && best_de == 0.0) break;
        if (clipped_lstar < lstar) low = mid;
        else high = mid;
    }
    return any;
}

/* The colour of an HCT hue, chroma and tone: that chroma, or the most sRGB has at that hue and tone. */
static uint32_t solve(double hue, double chroma, double lstar)
{
    double high = chroma, mid = chroma, low = 0.0;
    bool first = true, any = false;
    uint32_t answer = 0, possible = 0;
    if (chroma < 1.0 || lround(lstar) <= 0 || lround(lstar) >= 100) return argb_from_lstar(lstar);
    hue = hue < 0 ? 0 : hue > 360 ? 360 : hue;
    while (fabs(low - high) >= 0.4) {
        bool got = find_by_j(hue, mid, lstar, &possible);
        if (first) {
            if (got) return possible;
            first = false;
            mid = low + (high - low) / 2.0;
            continue;
        }
        if (!got) {
            high = mid;
        } else {
            answer = possible;
            any = true;
            low = mid;
        }
        mid = low + (high - low) / 2.0;
    }
    return any ? answer : argb_from_lstar(lstar);
}

uint32_t megler_palette_tone(uint32_t seed_argb, int palette, int tone)
{
    cam16 seed;
    double hue, chroma;
    make_viewing_conditions();
    seed = cam16_from_argb(seed_argb | 0xFF000000u);
    hue = seed.hue;
    switch (palette) {
    case MEGLER_PALETTE_NEUTRAL1: chroma = 4.0; break;
    case MEGLER_PALETTE_NEUTRAL2: chroma = 8.0; break;
    case MEGLER_PALETTE_ACCENT1: chroma = seed.chroma > 48.0 ? seed.chroma : 48.0; break;
    case MEGLER_PALETTE_ACCENT2: chroma = 16.0; break;
    case MEGLER_PALETTE_ACCENT3: hue = fmod(hue + 60.0, 360.0), chroma = 24.0; break;
    case MEGLER_PALETTE_ERROR: hue = 25.0, chroma = 84.0; break;
    default: return 0;
    }
    return solve(hue, chroma, (double)tone);
}
