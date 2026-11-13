// Overlay of core/framework/src/android/graphics/Color.java (tools/build-framework.sh).
// It differs in the colours of a colour space. The original knew sRGB alone: a packed long of another space was black,
// and the members that take or give a ColorSpace were signatures alone (colorSpace() and getColorSpace() null, pack and
// convert zero), which an app that asked for a ColorSpace.Rgb and went on to its colours would have met next.
// Here a Color object is its components and its space, and a packed long is what Android documents: an sRGB colour is
// the int in the high 32 bits, a colour of another named space is three half floats, ten bits of alpha and the space's
// id in the low six. android.util.Half is signatures alone here, so the two conversions of half floats are this file's.
// Everything else is the original's.
package android.graphics;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Locale;
import java.util.function.DoubleUnaryOperator;

public class Color {
    public static final int BLACK = 0xFF000000, DKGRAY = 0xFF444444, GRAY = 0xFF888888, LTGRAY = 0xFFCCCCCC, WHITE = 0xFFFFFFFF, RED = 0xFFFF0000,
        GREEN = 0xFF00FF00, BLUE = 0xFF0000FF, YELLOW = 0xFFFFFF00, CYAN = 0xFF00FFFF, MAGENTA = 0xFFFF00FF, TRANSPARENT = 0;
    /** The components of the colour in its space, then alpha. */
    private final float[] mComponents;
    private final ColorSpace mColorSpace;
    public Color() { this(0, 0, 0, 1); }
    private Color(float r, float g, float b, float a) { this(new float[] { r, g, b, a }, ColorSpace.get(ColorSpace.Named.SRGB)); }
    private Color(float[] components, ColorSpace colorSpace) { mComponents = components; mColorSpace = colorSpace; }
    public static Color valueOf(int c) { return new Color(red(c) / 255f, green(c) / 255f, blue(c) / 255f, alpha(c) / 255f); }
    public static Color valueOf(float r, float g, float b) { return new Color(r, g, b, 1); }
    public static Color valueOf(float r, float g, float b, float a) { return new Color(saturate(r), saturate(g), saturate(b), saturate(a)); }
    public static Color valueOf(long c) { return new Color(new float[] { red(c), green(c), blue(c), alpha(c) }, colorSpace(c)); }
    public static Color valueOf(float r, float g, float b, float a, ColorSpace colorSpace) {
        if (colorSpace.getComponentCount() > 3) throw new IllegalArgumentException("The specified color space must use a color model with at most 3 color components");
        return new Color(new float[] { r, g, b, a }, colorSpace);
    }
    public static Color valueOf(float[] components, ColorSpace colorSpace) {
        int count = colorSpace.getComponentCount() + 1;
        if (components.length < count)
            throw new IllegalArgumentException("Received a component array of length " + components.length + " but the color model requires " + count + " (including alpha)");
        return new Color(Arrays.copyOf(components, count), colorSpace);
    }
    public ColorSpace getColorSpace() { return mColorSpace; }
    public ColorSpace.Model getModel() { return mColorSpace.getModel(); }
    public boolean isWideGamut() { return mColorSpace.isWideGamut(); }
    public boolean isSrgb() { return mColorSpace.isSrgb(); }
    /** The components and alpha. */
    public int getComponentCount() { return mColorSpace.getComponentCount() + 1; }
    public float[] getComponents() { return Arrays.copyOf(mComponents, mComponents.length); }
    public float[] getComponents(float[] components) {
        if (components == null) return getComponents();
        if (components.length < mComponents.length) throw new IllegalArgumentException("The specified array's length must be at least " + mComponents.length);
        System.arraycopy(mComponents, 0, components, 0, mComponents.length);
        return components;
    }
    public float getComponent(int component) { return mComponents[component]; }
    public float red() { return mComponents[0]; } public float green() { return mComponents[1]; } public float blue() { return mComponents[2]; }
    public float alpha() { return mComponents[mComponents.length - 1]; }
    /** The colour in another space, by the perceptual intent. */
    public Color convert(ColorSpace colorSpace) {
        float[] c = Arrays.copyOf(mComponents, 4);
        ColorSpace.connect(mColorSpace, colorSpace).transform(c);
        return new Color(c, colorSpace);
    }
    /** As an sRGB int: a colour of another space is converted, and what sRGB cannot show is cut to what it can. */
    public int toArgb() {
        float[] c = Arrays.copyOf(mComponents, 4);
        if (!mColorSpace.isSrgb()) ColorSpace.connect(mColorSpace).transform(c);
        return argb(c[3], c[0], c[1], c[2]);
    }
    public long pack() { return pack(mComponents[0], mComponents[1], mComponents[2], mComponents[3], mColorSpace); }
    public float luminance() { return luminance(mColorSpace, mComponents[0], mComponents[1], mComponents[2]); }
    @Override public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        Color color = (Color) o;
        return Arrays.equals(mComponents, color.mComponents) && mColorSpace.equals(color.mColorSpace);
    }
    @Override public int hashCode() { return 31 * Arrays.hashCode(mComponents) + mColorSpace.hashCode(); }
    @Override public String toString() {
        StringBuilder b = new StringBuilder("Color(");
        for (float c : mComponents) b.append(c).append(", ");
        return b.append(mColorSpace.getName()).append(')').toString();
    }

    public static int alpha(int c) { return c >>> 24; }
    public static int red(int c) { return (c >> 16) & 0xFF; }
    public static int green(int c) { return (c >> 8) & 0xFF; }
    public static int blue(int c) { return c & 0xFF; }
    public static int rgb(int r, int g, int b) { return 0xFF000000 | (r << 16) | (g << 8) | b; }
    public static int rgb(float r, float g, float b) { return rgb((int) (r * 255 + 0.5f), (int) (g * 255 + 0.5f), (int) (b * 255 + 0.5f)); }
    public static int argb(int a, int r, int g, int b) { return (a << 24) | (r << 16) | (g << 8) | b; }
    public static int argb(float a, float r, float g, float b) { return argb((int) (a * 255 + 0.5f), (int) (r * 255 + 0.5f), (int) (g * 255 + 0.5f), (int) (b * 255 + 0.5f)); }

    // ---- packed long colours. The low six bits are the id of a named colour space. Of sRGB (id 0) the high 32 bits are
    // the int; of another space the bits from the top are red, green and blue as half floats and ten bits of alpha.

    public static long pack(int c) { return ((long) c) << 32; }
    public static long pack(float r, float g, float b) { return pack(rgb(r, g, b)); }
    public static long pack(float r, float g, float b, float a) { return pack(argb(a, r, g, b)); }
    public static long pack(float r, float g, float b, float a, ColorSpace colorSpace) {
        if (colorSpace.isSrgb()) return pack(argb(a, r, g, b));
        int id = colorSpace.getId();
        if (id == ColorSpace.MIN_ID) throw new IllegalArgumentException("Unknown color space, please use a color space returned by ColorSpace.get()");
        if (colorSpace.getComponentCount() > 3) throw new IllegalArgumentException("The color space must use a color model with at most 3 components");
        long alpha = (long) (saturate(a) * 1023.0f + 0.5f);
        return (long) toHalf(r) << 48 | (long) toHalf(g) << 32 | (long) toHalf(b) << 16 | (alpha & 0x3ffL) << 6 | (id & 0x3fL);
    }
    public static ColorSpace colorSpace(long c) { return ColorSpace.get((int) (c & 0x3fL)); }
    public static boolean isSrgb(long c) { return colorSpace(c).isSrgb(); }
    public static boolean isWideGamut(long c) { return colorSpace(c).isWideGamut(); }
    public static boolean isInColorSpace(long c, ColorSpace colorSpace) { return (int) (c & 0x3fL) == colorSpace.getId(); }
    public static float red(long c) { return (c & 0x3fL) == 0 ? ((c >> 48) & 0xff) / 255f : fromHalf((int) (c >> 48) & 0xffff); }
    public static float green(long c) { return (c & 0x3fL) == 0 ? ((c >> 40) & 0xff) / 255f : fromHalf((int) (c >> 32) & 0xffff); }
    public static float blue(long c) { return (c & 0x3fL) == 0 ? ((c >> 32) & 0xff) / 255f : fromHalf((int) (c >> 16) & 0xffff); }
    public static float alpha(long c) { return (c & 0x3fL) == 0 ? ((c >> 56) & 0xff) / 255f : ((c >> 6) & 0x3ff) / 1023f; }
    /** As an sRGB int: a colour of another space is converted, and what sRGB cannot show is cut to what it can. */
    public static int toArgb(long c) {
        if ((c & 0x3fL) == 0) return (int) (c >> 32);
        float[] v = ColorSpace.connect(colorSpace(c)).transform(red(c), green(c), blue(c));
        return argb(alpha(c), v[0], v[1], v[2]);
    }
    static int[] meglerInts(long[] c) { int[] o = new int[c.length]; for (int i = 0; i < c.length; i++) o[i] = toArgb(c[i]); return o; }
    public static long convert(int c, ColorSpace colorSpace) {
        return convert(red(c) / 255f, green(c) / 255f, blue(c) / 255f, alpha(c) / 255f, ColorSpace.get(ColorSpace.Named.SRGB), colorSpace);
    }
    public static long convert(long c, ColorSpace colorSpace) { return convert(red(c), green(c), blue(c), alpha(c), colorSpace(c), colorSpace); }
    public static long convert(float r, float g, float b, float a, ColorSpace source, ColorSpace destination) {
        return convert(r, g, b, a, ColorSpace.connect(source, destination));
    }
    public static long convert(long c, ColorSpace.Connector connector) { return convert(red(c), green(c), blue(c), alpha(c), connector); }
    public static long convert(float r, float g, float b, float a, ColorSpace.Connector connector) {
        float[] v = connector.transform(r, g, b);
        return pack(v[0], v[1], v[2], a, connector.getDestination());
    }
    public static float luminance(long c) { return luminance(colorSpace(c), red(c), green(c), blue(c)); }
    public static float luminance(int c) {
        double r = lin(red(c) / 255.0), g = lin(green(c) / 255.0), b = lin(blue(c) / 255.0);
        return (float) (0.2126 * r + 0.7152 * g + 0.0722 * b);
    }
    private static double lin(double v) { return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); }
    /** The relative luminance of a colour of an RGB space: of its light, which its transfer function gives. */
    private static float luminance(ColorSpace colorSpace, float r, float g, float b) {
        if (colorSpace.getModel() != ColorSpace.Model.RGB)
            throw new IllegalArgumentException("The specified color must be encoded in an RGB color space. The supplied color space is " + colorSpace.getModel());
        DoubleUnaryOperator eotf = ((ColorSpace.Rgb) colorSpace).getEotf();
        return saturate((float) (0.2126 * eotf.applyAsDouble(r) + 0.7152 * eotf.applyAsDouble(g) + 0.0722 * eotf.applyAsDouble(b)));
    }
    private static float saturate(float v) { return v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v; }
    /** A float as the 16 bits of a half float (a sign, 5 bits of exponent, 10 of fraction), rounded to the nearest. */
    private static int toHalf(float f) {
        int bits = Float.floatToRawIntBits(f), sign = (bits >>> 16) & 0x8000, e = (bits >>> 23) & 0xff, m = bits & 0x7fffff;
        if (e == 0xff) return sign | 0x7c00 | (m != 0 ? 0x200 : 0);          // infinity, or not a number
        e = e - 127 + 15;
        if (e >= 0x1f) return sign | 0x7c00;                                 // too large: infinity
        if (e <= 0) {
            if (e < -10) return sign;                                        // too small: zero
            m = (m | 0x800000) >> (1 - e);                                   // below the least normal half: one without an exponent
            if ((m & 0x1000) != 0) m += 0x2000;
            return sign | (m >> 13);
        }
        int half = (e << 10) | (m >> 13);
        if ((m & 0x1000) != 0) half++;                                       // rounding up may carry into the exponent, as it should
        return sign | half;
    }
    /** And the float of 16 such bits. */
    private static float fromHalf(int h) {
        int e = (h >>> 10) & 0x1f, m = h & 0x3ff;
        float v;
        if (e == 0) v = m / 16777216f;                                       // no exponent: fraction times 2^-24
        else if (e == 0x1f) v = m != 0 ? Float.NaN : Float.POSITIVE_INFINITY;
        else v = Float.intBitsToFloat(((e - 15 + 127) << 23) | (m << 13));
        return (h & 0x8000) != 0 ? -v : v;
    }

    private static final HashMap<String, Integer> sNames = new HashMap<>();
    static {
        sNames.put("black", BLACK); sNames.put("darkgray", DKGRAY); sNames.put("gray", GRAY); sNames.put("lightgray", LTGRAY); sNames.put("white", WHITE);
        sNames.put("red", RED); sNames.put("green", GREEN); sNames.put("blue", BLUE); sNames.put("yellow", YELLOW); sNames.put("cyan", CYAN);
        sNames.put("magenta", MAGENTA); sNames.put("aqua", 0xFF00FFFF); sNames.put("fuchsia", 0xFFFF00FF); sNames.put("darkgrey", DKGRAY);
        sNames.put("grey", GRAY); sNames.put("lightgrey", LTGRAY); sNames.put("lime", 0xFF00FF00); sNames.put("maroon", 0xFF800000);
        sNames.put("navy", 0xFF000080); sNames.put("olive", 0xFF808000); sNames.put("purple", 0xFF800080); sNames.put("silver", 0xFFC0C0C0);
        sNames.put("teal", 0xFF008080);
    }
    public static int parseColor(String s) {
        if (s.length() > 0 && s.charAt(0) == '#') {
            long v = Long.parseLong(s.substring(1), 16);
            if (s.length() == 7) v |= 0xFF000000L;
            else if (s.length() != 9) throw new IllegalArgumentException("Unknown color");
            return (int) v;
        }
        Integer c = sNames.get(s.toLowerCase(Locale.ROOT));
        if (c != null) return c;
        throw new IllegalArgumentException("Unknown color");
    }
    public static void RGBToHSV(int r, int g, int b, float[] hsv) {
        float rf = r / 255f, gf = g / 255f, bf = b / 255f, max = Math.max(rf, Math.max(gf, bf)), min = Math.min(rf, Math.min(gf, bf)), d = max - min;
        float h = 0;
        if (d != 0) {
            if (max == rf) h = 60 * (((gf - bf) / d) % 6);
            else if (max == gf) h = 60 * ((bf - rf) / d + 2);
            else h = 60 * ((rf - gf) / d + 4);
        }
        if (h < 0) h += 360;
        hsv[0] = h; hsv[1] = max == 0 ? 0 : d / max; hsv[2] = max;
    }
    public static void colorToHSV(int c, float[] hsv) { RGBToHSV(red(c), green(c), blue(c), hsv); }
    public static int HSVToColor(float[] hsv) { return HSVToColor(0xFF, hsv); }
    public static int HSVToColor(int alpha, float[] hsv) {
        float h = ((hsv[0] % 360) + 360) % 360, s = Math.max(0, Math.min(1, hsv[1])), v = Math.max(0, Math.min(1, hsv[2]));
        float c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c, r, g, b;
        if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
        else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
        return argb(alpha, Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255));
    }
}
