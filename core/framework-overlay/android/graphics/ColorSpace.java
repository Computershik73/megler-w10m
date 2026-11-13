// Overlay of core/framework/src/android/graphics/ColorSpace.java (tools/build-framework.sh).
// The original was one nameless sRGB for every name: get() gave an object that was no ColorSpace.Rgb, and Rgb, Connector,
// connect, adapt and match were signatures alone. YouTube casts the space it gets to Rgb while its settings are laid out,
// and the screen ended in a ClassCastException.
// This one is the class as Android documents it: the named spaces with their primaries, white points and transfer
// functions (the RGB ones are ColorSpace.Rgb, with CIE XYZ and CIE L*a*b* beside them), conversion to and from XYZ,
// connectors between two spaces through XYZ of D50 with chromatic adaptation, adapt, match, and the data space numbers.
// Matrices are of 9 floats, column after column, as Android gives and takes them.
package android.graphics;

import java.util.Arrays;
import java.util.function.DoubleUnaryOperator;

public abstract class ColorSpace {
    public static final float[] ILLUMINANT_A = { 0.44757f, 0.40745f };
    public static final float[] ILLUMINANT_B = { 0.34842f, 0.35161f };
    public static final float[] ILLUMINANT_C = { 0.31006f, 0.31616f };
    public static final float[] ILLUMINANT_D50 = { 0.34567f, 0.35850f };
    public static final float[] ILLUMINANT_D55 = { 0.33242f, 0.34743f };
    public static final float[] ILLUMINANT_D60 = { 0.32168f, 0.33767f };
    public static final float[] ILLUMINANT_D65 = { 0.31271f, 0.32902f };
    public static final float[] ILLUMINANT_D75 = { 0.29902f, 0.31485f };
    public static final float[] ILLUMINANT_E = { 0.33333f, 0.33333f };
    public static final int MIN_ID = -1, MAX_ID = 63;

    public enum Named { SRGB, LINEAR_SRGB, EXTENDED_SRGB, LINEAR_EXTENDED_SRGB, BT709, BT2020, DCI_P3, DISPLAY_P3, NTSC_1953, SMPTE_C, ADOBE_RGB, PRO_PHOTO_RGB, ACES, ACESCG, CIE_XYZ, CIE_LAB, BT2020_HLG, BT2020_PQ }
    public enum RenderIntent { PERCEPTUAL, RELATIVE, SATURATION, ABSOLUTE }
    /** The cone responses a white point is adapted in: each a matrix from XYZ. */
    public enum Adaptation {
        BRADFORD(new float[] { 0.8951f, -0.7502f, 0.0389f, 0.2664f, 1.7135f, -0.0685f, -0.1614f, 0.0367f, 1.0296f }),
        VON_KRIES(new float[] { 0.40024f, -0.22630f, 0.00000f, 0.70760f, 1.16532f, 0.00000f, -0.08081f, 0.04570f, 0.91822f }),
        CIECAT02(new float[] { 0.7328f, -0.7036f, 0.0030f, 0.4296f, 1.6975f, 0.0136f, -0.1624f, 0.0061f, 0.9834f });
        final float[] mTransform;
        Adaptation(float[] transform) { mTransform = transform; }
    }
    public enum Model {
        RGB(3), XYZ(3), LAB(3), CMYK(4);
        private final int mComponentCount;
        Model(int componentCount) { mComponentCount = componentCount; }
        public int getComponentCount() { return mComponentCount; }
    }

    private static final float[] SRGB_PRIMARIES = { 0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f };
    private static final float[] NTSC_1953_PRIMARIES = { 0.67f, 0.33f, 0.21f, 0.71f, 0.14f, 0.08f };
    private static final float[] BT2020_PRIMARIES = { 0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f };
    private static final float[] P3_PRIMARIES = { 0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f };
    private static final float[] GRAY_PRIMARIES = { 1f, 1f, 1f, 1f, 1f, 1f };
    private static final float[] D50_XYZ = { 0.964212f, 1.0f, 0.825188f };
    private static final ColorSpace[] sNamed = new ColorSpace[Named.values().length];
    /** The data spaces (android.hardware.DataSpace) of the named spaces that have one: the number, then the name's place. */
    private static final int[] DATA_SPACES = {
        142671872, Named.SRGB.ordinal(), 138477568, Named.LINEAR_SRGB.ordinal(), 411107328, Named.EXTENDED_SRGB.ordinal(),
        406913024, Named.LINEAR_EXTENDED_SRGB.ordinal(), 281083904, Named.BT709.ordinal(), 147193856, Named.BT2020.ordinal(),
        155844608, Named.DCI_P3.ordinal(), 143261696, Named.DISPLAY_P3.ordinal(), 151715840, Named.ADOBE_RGB.ordinal(),
        168165376, Named.BT2020_HLG.ordinal(), 163971072, Named.BT2020_PQ.ordinal(),
    };

    static {
        final Rgb.TransferParameters srgb = new Rgb.TransferParameters(1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045, 2.4);
        final Rgb.TransferParameters video = new Rgb.TransferParameters(1 / 1.099, 0.099 / 1.099, 1 / 4.5, 0.081, 1 / 0.45);
        // Hybrid log-gamma and the perceptual quantizer, written as Skia writes them, 1 being the white of a plain picture.
        final Rgb.TransferParameters hlg = new Rgb.TransferParameters(2.0, 2.0, 1 / 0.17883277, 0.28466892, 0.55991073, -0.685490157, Rgb.TransferParameters.TYPE_HLGISH);
        final Rgb.TransferParameters pq = new Rgb.TransferParameters(-1.555223, 1.860454, 32 / 2523.0, 2413 / 128.0, -2392 / 128.0, 8192 / 1305.0, Rgb.TransferParameters.TYPE_PQISH);
        // sRGB first: the others are compared with it.
        put(Named.SRGB, new Rgb("sRGB IEC61966-2.1", SRGB_PRIMARIES, ILLUMINANT_D65, null, srgb, Named.SRGB.ordinal()));
        put(Named.LINEAR_SRGB, new Rgb("sRGB IEC61966-2.1 (Linear)", SRGB_PRIMARIES, ILLUMINANT_D65, 1.0, 0.0f, 1.0f, Named.LINEAR_SRGB.ordinal()));
        put(Named.EXTENDED_SRGB, new Rgb("scRGB-nl IEC 61966-2-2:2003", SRGB_PRIMARIES, ILLUMINANT_D65, null,
            x -> Math.copySign(Rgb.encode(srgb, Math.abs(x)), x), x -> Math.copySign(Rgb.decode(srgb, Math.abs(x)), x), -0.799f, 2.399f, srgb, Named.EXTENDED_SRGB.ordinal()));
        put(Named.LINEAR_EXTENDED_SRGB, new Rgb("scRGB IEC 61966-2-2:2003", SRGB_PRIMARIES, ILLUMINANT_D65, 1.0, -0.5f, 7.499f, Named.LINEAR_EXTENDED_SRGB.ordinal()));
        put(Named.BT709, new Rgb("Rec. ITU-R BT.709-5", SRGB_PRIMARIES, ILLUMINANT_D65, null, video, Named.BT709.ordinal()));
        put(Named.BT2020, new Rgb("Rec. ITU-R BT.2020-1", BT2020_PRIMARIES, ILLUMINANT_D65, null,
            new Rgb.TransferParameters(1 / 1.0993, 0.0993 / 1.0993, 1 / 4.5, 0.08145, 1 / 0.45), Named.BT2020.ordinal()));
        put(Named.DCI_P3, new Rgb("SMPTE RP 431-2-2007 DCI (P3)", P3_PRIMARIES, new float[] { 0.314f, 0.351f }, 2.6, 0.0f, 1.0f, Named.DCI_P3.ordinal()));
        put(Named.DISPLAY_P3, new Rgb("Display P3", P3_PRIMARIES, ILLUMINANT_D65, null, srgb, Named.DISPLAY_P3.ordinal()));
        put(Named.NTSC_1953, new Rgb("NTSC (1953)", NTSC_1953_PRIMARIES, ILLUMINANT_C, null, video, Named.NTSC_1953.ordinal()));
        put(Named.SMPTE_C, new Rgb("SMPTE-C RGB", new float[] { 0.630f, 0.340f, 0.310f, 0.595f, 0.155f, 0.070f }, ILLUMINANT_D65, null, video, Named.SMPTE_C.ordinal()));
        put(Named.ADOBE_RGB, new Rgb("Adobe RGB (1998)", new float[] { 0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f }, ILLUMINANT_D65, 2.2, 0.0f, 1.0f, Named.ADOBE_RGB.ordinal()));
        put(Named.PRO_PHOTO_RGB, new Rgb("ROMM RGB ISO 22028-2:2013", new float[] { 0.7347f, 0.2653f, 0.1596f, 0.8404f, 0.0366f, 0.0001f }, ILLUMINANT_D50, null,
            new Rgb.TransferParameters(1.0, 0.0, 1 / 16.0, 0.031248, 1.8), Named.PRO_PHOTO_RGB.ordinal()));
        put(Named.ACES, new Rgb("SMPTE ST 2065-1:2012 ACES", new float[] { 0.73470f, 0.26530f, 0.0f, 1.0f, 0.00010f, -0.0770f }, ILLUMINANT_D60, 1.0, -65504.0f, 65504.0f, Named.ACES.ordinal()));
        put(Named.ACESCG, new Rgb("Academy S-2014-004 ACEScg", new float[] { 0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f }, ILLUMINANT_D60, 1.0, -65504.0f, 65504.0f, Named.ACESCG.ordinal()));
        put(Named.CIE_XYZ, new Xyz("Generic XYZ", Named.CIE_XYZ.ordinal()));
        put(Named.CIE_LAB, new Lab("Generic L*a*b*", Named.CIE_LAB.ordinal()));
        put(Named.BT2020_HLG, new Rgb("Hybrid Log Gamma encoding", BT2020_PRIMARIES, ILLUMINANT_D65, null, hlg, Named.BT2020_HLG.ordinal()));
        put(Named.BT2020_PQ, new Rgb("Perceptual Quantizer encoding", BT2020_PRIMARIES, ILLUMINANT_D65, null, pq, Named.BT2020_PQ.ordinal()));
    }
    private static void put(Named name, ColorSpace space) { sNamed[name.ordinal()] = space; }

    private final String mName;
    private final Model mModel;
    private final int mId;

    ColorSpace(String name, Model model, int id) {
        if (name == null || name.length() < 1) throw new IllegalArgumentException("The name of a color space cannot be null and must contain at least 1 character");
        if (model == null) throw new IllegalArgumentException("A color space must have a model");
        if (id < MIN_ID || id > MAX_ID) throw new IllegalArgumentException("The id must be between " + MIN_ID + " and " + MAX_ID);
        mName = name;
        mModel = model;
        mId = id;
    }

    public String getName() { return mName; }
    /** The place among the named spaces, or MIN_ID for a space an app made. */
    public int getId() { return mId; }
    public Model getModel() { return mModel; }
    public int getComponentCount() { return mModel.getComponentCount(); }
    public abstract boolean isWideGamut();
    public boolean isSrgb() { return false; }
    public abstract float getMinValue(int component);
    public abstract float getMaxValue(int component);
    public float[] toXyz(float r, float g, float b) { return toXyz(new float[] { r, g, b }); }
    /** The colour as XYZ, in the array it came in. */
    public abstract float[] toXyz(float[] v);
    public float[] fromXyz(float x, float y, float z) {
        float[] v = new float[mModel.getComponentCount()];
        v[0] = x; v[1] = y; v[2] = z;
        return fromXyz(v);
    }
    /** The XYZ colour as one of this space, in the array it came in. */
    public abstract float[] fromXyz(float[] v);
    public int getDataSpace() {
        for (int i = 0; i < DATA_SPACES.length; i += 2) if (DATA_SPACES[i + 1] == mId) return DATA_SPACES[i];
        return 0;
    }
    @Override public String toString() { return mName + " (id=" + mId + ", model=" + mModel + ")"; }
    @Override public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        ColorSpace that = (ColorSpace) o;
        return mId == that.mId && mName.equals(that.mName) && mModel == that.mModel;
    }
    @Override public int hashCode() { return (mName.hashCode() * 31 + mModel.hashCode()) * 31 + mId; }

    public static ColorSpace get(Named name) { return sNamed[name.ordinal()]; }
    /** The named space of an id, as a packed colour carries it. */
    static ColorSpace get(int id) {
        if (id < 0 || id >= sNamed.length) throw new IllegalArgumentException("Invalid ID, must be in the range [0.." + sNamed.length + ")");
        return sNamed[id];
    }
    public static ColorSpace getFromDataSpace(int dataSpace) {
        for (int i = 0; i < DATA_SPACES.length; i += 2) if (DATA_SPACES[i] == dataSpace) return sNamed[DATA_SPACES[i + 1]];
        return null;
    }
    /** The named RGB space of a transform to XYZ of D50 and a transfer function, or null. */
    public static ColorSpace match(float[] toXYZD50, Rgb.TransferParameters function) {
        for (ColorSpace space : sNamed) {
            if (space.getModel() != Model.RGB) continue;
            Rgb rgb = (Rgb) adapt(space, D50_XYZ);
            if (near(toXYZD50, rgb.mTransform) && near(function, rgb.mTransferParameters)) return space;
        }
        return null;
    }

    public static Connector connect(ColorSpace source, ColorSpace destination) { return connect(source, destination, RenderIntent.PERCEPTUAL); }
    public static Connector connect(ColorSpace source, ColorSpace destination, RenderIntent intent) {
        if (source.equals(destination)) return Connector.identity(source);
        if (source.getModel() == Model.RGB && destination.getModel() == Model.RGB) return new RgbConnector((Rgb) source, (Rgb) destination, intent);
        return new Connector(source, destination, intent);
    }
    public static Connector connect(ColorSpace source) { return connect(source, RenderIntent.PERCEPTUAL); }
    /** To sRGB. */
    public static Connector connect(ColorSpace source, RenderIntent intent) {
        if (source.isSrgb()) return Connector.identity(source);
        if (source.getModel() == Model.RGB) return new RgbConnector((Rgb) source, (Rgb) get(Named.SRGB), intent);
        return new Connector(source, get(Named.SRGB), intent);
    }

    public static ColorSpace adapt(ColorSpace colorSpace, float[] whitePoint) { return adapt(colorSpace, whitePoint, Adaptation.BRADFORD); }
    /** An RGB space with another white point (xy, or XYZ): its transform adapted, the rest its own. Other spaces as they are. */
    public static ColorSpace adapt(ColorSpace colorSpace, float[] whitePoint, Adaptation adaptation) {
        if (colorSpace.getModel() != Model.RGB) return colorSpace;
        Rgb rgb = (Rgb) colorSpace;
        if (near(rgb.mWhitePoint, Rgb.xy(whitePoint))) return colorSpace;
        float[] to = whitePoint.length == 3 ? Arrays.copyOf(whitePoint, 3) : xyToXyz(whitePoint);
        float[] adapted = mul(adaptation(adaptation.mTransform, xyToXyz(rgb.mWhitePoint), to), rgb.mTransform);
        return new Rgb(rgb.getName(), rgb.mPrimaries, whitePoint, adapted, rgb.mOetf, rgb.mEotf, rgb.mMin, rgb.mMax, rgb.mTransferParameters, MIN_ID);
    }
    /** The matrix that takes XYZ colours seen under one white point to those seen under another. */
    public static float[] chromaticAdaptation(Adaptation adaptation, float[] srcWhitePoint, float[] dstWhitePoint) {
        if ((srcWhitePoint.length != 2 && srcWhitePoint.length != 3) || (dstWhitePoint.length != 2 && dstWhitePoint.length != 3))
            throw new IllegalArgumentException("A white point array must have 2 or 3 floats");
        float[] from = srcWhitePoint.length == 3 ? Arrays.copyOf(srcWhitePoint, 3) : xyToXyz(srcWhitePoint);
        float[] to = dstWhitePoint.length == 3 ? Arrays.copyOf(dstWhitePoint, 3) : xyToXyz(dstWhitePoint);
        if (near(from, to)) return new float[] { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        return adaptation(adaptation.mTransform, from, to);
    }
    /** The white of a black body of a temperature in kelvins, as XYZ (Kim's approximation of the Planckian locus). */
    public static float[] cctToXyz(int cct) {
        if (cct < 1) throw new IllegalArgumentException("Temperature must be greater than 0");
        float t = 1e3f / cct, t2 = t * t;
        float x = cct <= 4000 ? 0.179910f + 0.8776956f * t - 0.2343589f * t2 - 0.2661239f * t2 * t
                              : 0.240390f + 0.2226347f * t + 2.1070379f * t2 - 3.0258469f * t2 * t;
        float x2 = x * x;
        float y = cct <= 2222 ? -0.20219683f + 2.18555832f * x - 1.34811020f * x2 - 1.1063814f * x2 * x
                : cct <= 4000 ? -0.16748867f + 2.09137015f * x - 1.37418593f * x2 - 0.9549476f * x2 * x
                              : -0.37001483f + 3.75112997f * x - 5.8733867f * x2 + 3.0817580f * x2 * x;
        return xyToXyz(new float[] { x, y });
    }

    // ---- the arithmetic: matrices of 9 floats, column after column

    /** Whether two arrays are the same to a thousandth, as far as the shorter goes. */
    private static boolean near(float[] a, float[] b) {
        if (a == b) return true;
        if (a == null || b == null) return false;
        for (int i = 0; i < a.length && i < b.length; i++) if (Float.compare(a[i], b[i]) != 0 && Math.abs(a[i] - b[i]) > 1e-3f) return false;
        return true;
    }
    private static boolean near(Rgb.TransferParameters a, Rgb.TransferParameters b) {
        if (a == null || b == null) return a == b;
        return Math.abs(a.a - b.a) < 1e-3 && Math.abs(a.b - b.b) < 1e-3 && Math.abs(a.c - b.c) < 1e-3 && Math.abs(a.d - b.d) < 2e-3
            && Math.abs(a.e - b.e) < 1e-3 && Math.abs(a.f - b.f) < 1e-3 && Math.abs(a.g - b.g) < 1e-3;
    }
    /** A chromaticity as XYZ of luminance 1. */
    private static float[] xyToXyz(float[] xy) { return new float[] { xy[0] / xy[1], 1.0f, (1 - xy[0] - xy[1]) / xy[1] }; }
    private static float[] mul(float[] l, float[] r) {
        float[] o = new float[9];
        for (int c = 0; c < 3; c++) for (int row = 0; row < 3; row++) o[c * 3 + row] = l[row] * r[c * 3] + l[3 + row] * r[c * 3 + 1] + l[6 + row] * r[c * 3 + 2];
        return o;
    }
    /** A matrix times a vector, into the vector. */
    private static float[] mulVec(float[] m, float[] v) {
        float x = v[0], y = v[1], z = v[2];
        v[0] = m[0] * x + m[3] * y + m[6] * z;
        v[1] = m[1] * x + m[4] * y + m[7] * z;
        v[2] = m[2] * x + m[5] * y + m[8] * z;
        return v;
    }
    /** A diagonal matrix, given as its diagonal, times a matrix. */
    private static float[] mulDiag(float[] d, float[] m) {
        float[] o = new float[9];
        for (int i = 0; i < 9; i++) o[i] = d[i % 3] * m[i];
        return o;
    }
    private static float[] inverse(float[] m) {
        float a = m[0], b = m[3], c = m[6], d = m[1], e = m[4], f = m[7], g = m[2], h = m[5], i = m[8];
        float ca = e * i - f * h, cb = f * g - d * i, cc = d * h - e * g, det = a * ca + b * cb + c * cc;
        float[] o = new float[9];
        o[0] = ca / det; o[1] = cb / det; o[2] = cc / det;
        o[3] = (c * h - b * i) / det; o[4] = (a * i - c * g) / det; o[5] = (b * g - a * h) / det;
        o[6] = (b * f - c * e) / det; o[7] = (c * d - a * f) / det; o[8] = (a * e - b * d) / det;
        return o;
    }
    /** Von Kries's adaptation in the cone responses of `cone`: back from them, scaled by the two whites' ratio, into them. */
    private static float[] adaptation(float[] cone, float[] srcWhite, float[] dstWhite) {
        float[] from = mulVec(cone, Arrays.copyOf(srcWhite, 3)), to = mulVec(cone, Arrays.copyOf(dstWhite, 3));
        return mul(inverse(cone), mulDiag(new float[] { to[0] / from[0], to[1] / from[1], to[2] / from[2] }, cone));
    }

    /** An RGB space: three primaries and a white point, which make its transform to XYZ, and a transfer function. */
    public static class Rgb extends ColorSpace {
        /** A transfer function of the form Y = (aX + b)^g + e from d up and cX + f below it; or, by the marks in g, the
         *  perceptual quantizer's or hybrid log-gamma's with the six others as Skia packs them. */
        public static class TransferParameters {
            static final double TYPE_PQISH = -2.0, TYPE_HLGISH = -3.0;
            public final double a, b, c, d, e, f, g;
            public TransferParameters(double a, double b, double c, double d, double g) { this(a, b, c, d, 0.0, 0.0, g); }
            public TransferParameters(double a, double b, double c, double d, double e, double f, double g) {
                if (Double.isNaN(a) || Double.isNaN(b) || Double.isNaN(c) || Double.isNaN(d) || Double.isNaN(e) || Double.isNaN(f) || Double.isNaN(g))
                    throw new IllegalArgumentException("Parameters cannot be NaN");
                if (g != TYPE_PQISH && g != TYPE_HLGISH) {
                    if (!(d >= 0.0 && d <= 1.0f + Math.ulp(1.0f))) throw new IllegalArgumentException("Parameter d must be in the range [0..1], was " + d);
                    if (d == 0.0 && (a == 0.0 || g == 0.0)) throw new IllegalArgumentException("Parameter a or g is zero, the transfer function is constant");
                    if (d >= 1.0 && c == 0.0) throw new IllegalArgumentException("Parameter c is zero, the transfer function is constant");
                    if ((a == 0.0 || g == 0.0) && c == 0.0) throw new IllegalArgumentException("Parameter a or g is zero, and c is zero, the transfer function is constant");
                    if (c < 0.0) throw new IllegalArgumentException("The transfer function must be increasing");
                    if (a < 0.0 || g < 0.0) throw new IllegalArgumentException("The transfer function must be positive or increasing");
                }
                this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f; this.g = g;
            }
            @Override public boolean equals(Object o) {
                if (this == o) return true;
                if (o == null || getClass() != o.getClass()) return false;
                TransferParameters t = (TransferParameters) o;
                return Double.compare(t.a, a) == 0 && Double.compare(t.b, b) == 0 && Double.compare(t.c, c) == 0 && Double.compare(t.d, d) == 0
                    && Double.compare(t.e, e) == 0 && Double.compare(t.f, f) == 0 && Double.compare(t.g, g) == 0;
            }
            @Override public int hashCode() { return Arrays.hashCode(new double[] { a, b, c, d, e, f, g }); }
        }

        private final float[] mWhitePoint, mPrimaries, mTransform, mInverseTransform;
        private final DoubleUnaryOperator mOetf, mEotf, mClampedOetf, mClampedEotf;
        private final float mMin, mMax;
        private final boolean mIsWideGamut, mIsSrgb;
        private final TransferParameters mTransferParameters;

        public Rgb(String name, float[] toXYZ, DoubleUnaryOperator oetf, DoubleUnaryOperator eotf) {
            this(name, primariesOf(toXYZ), whitePointOf(toXYZ), null, oetf, eotf, 0.0f, 1.0f, null, MIN_ID);
        }
        public Rgb(String name, float[] primaries, float[] whitePoint, DoubleUnaryOperator oetf, DoubleUnaryOperator eotf, float min, float max) {
            this(name, primaries, whitePoint, null, oetf, eotf, min, max, null, MIN_ID);
        }
        public Rgb(String name, float[] toXYZ, TransferParameters function) {
            // A matrix of a diagonal alone is a grey space: it has no primaries to find, and is kept as it is.
            this(name, isGray(toXYZ) ? GRAY_PRIMARIES : primariesOf(toXYZ), whitePointOf(toXYZ), isGray(toXYZ) ? toXYZ : null, function, MIN_ID);
        }
        public Rgb(String name, float[] primaries, float[] whitePoint, TransferParameters function) { this(name, primaries, whitePoint, null, function, MIN_ID); }
        public Rgb(String name, float[] toXYZ, double gamma) { this(name, primariesOf(toXYZ), whitePointOf(toXYZ), gamma, 0.0f, 1.0f, MIN_ID); }
        public Rgb(String name, float[] primaries, float[] whitePoint, double gamma) { this(name, primaries, whitePoint, gamma, 0.0f, 1.0f, MIN_ID); }

        Rgb(String name, float[] primaries, float[] whitePoint, float[] transform, TransferParameters function, int id) {
            this(name, primaries, whitePoint, transform, oetfOf(function), eotfOf(function), 0.0f, 1.0f, function, id);
        }
        Rgb(String name, float[] primaries, float[] whitePoint, final double gamma, float min, float max, int id) {
            this(name, primaries, whitePoint, null, gamma == 1.0 ? x -> x : x -> Math.pow(x < 0.0 ? 0.0 : x, 1 / gamma),
                gamma == 1.0 ? x -> x : x -> Math.pow(x < 0.0 ? 0.0 : x, gamma), min, max, new TransferParameters(1.0, 0.0, 0.0, 0.0, gamma), id);
        }
        Rgb(String name, float[] primaries, float[] whitePoint, float[] transform, DoubleUnaryOperator oetf, DoubleUnaryOperator eotf, float min, float max,
                TransferParameters transferParameters, int id) {
            super(name, Model.RGB, id);
            if (primaries == null || (primaries.length != 6 && primaries.length != 9))
                throw new IllegalArgumentException("The color space's primaries must be defined as an array of 6 floats in xyY or 9 floats in XYZ");
            if (whitePoint == null || (whitePoint.length != 2 && whitePoint.length != 3))
                throw new IllegalArgumentException("The color space's white point must be defined as an array of 2 floats in xyY or 3 float in XYZ");
            if (oetf == null || eotf == null) throw new IllegalArgumentException("The transfer functions of a color space cannot be null");
            if (min >= max) throw new IllegalArgumentException("Invalid range: min=" + min + ", max=" + max + "; min must be strictly < max");
            if (transform != null && transform.length != 9) throw new IllegalArgumentException("Transform must have 9 entries! Has " + transform.length);
            mWhitePoint = xy(whitePoint);
            mPrimaries = primaries.length == 6 ? Arrays.copyOf(primaries, 6) : new float[] { xy(primaries, 0)[0], xy(primaries, 0)[1], xy(primaries, 3)[0], xy(primaries, 3)[1], xy(primaries, 6)[0], xy(primaries, 6)[1] };
            mTransform = transform != null ? Arrays.copyOf(transform, 9) : toXyzOf(mPrimaries, mWhitePoint);
            mInverseTransform = inverse(mTransform);
            mOetf = oetf;
            mEotf = eotf;
            mMin = min;
            mMax = max;
            mClampedOetf = x -> clamp(oetf.applyAsDouble(x));
            mClampedEotf = x -> eotf.applyAsDouble(clamp(x));
            mTransferParameters = transferParameters;
            mIsWideGamut = (area(mPrimaries) / area(NTSC_1953_PRIMARIES) > 0.9f && holds(mPrimaries, SRGB_PRIMARIES)) || (min < 0.0f && max > 1.0f);
            mIsSrgb = id == 0 || sameAsSrgb();
        }

        private double clamp(double x) { return x < mMin ? mMin : x > mMax ? mMax : x; }
        /** Whether a space that is not the named sRGB is one all the same: its primaries, white point and range, and its
         *  transfer functions at each of 256 steps. */
        private boolean sameAsSrgb() {
            if (!(sNamed[0] instanceof Rgb) || !near(mPrimaries, SRGB_PRIMARIES) || !near(mWhitePoint, ILLUMINANT_D65) || mMin != 0.0f || mMax != 1.0f) return false;
            Rgb srgb = (Rgb) sNamed[0];
            for (double x = 0.0; x <= 1.0; x += 1 / 255.0) {
                if (Math.abs(mOetf.applyAsDouble(x) - srgb.mOetf.applyAsDouble(x)) > 1e-3) return false;
                if (Math.abs(mEotf.applyAsDouble(x) - srgb.mEotf.applyAsDouble(x)) > 1e-3) return false;
            }
            return true;
        }

        /** A white point, or one colour of three in XYZ, as a chromaticity. */
        static float[] xy(float[] v) { return v.length == 2 ? Arrays.copyOf(v, 2) : xy(v, 0); }
        private static float[] xy(float[] xyz, int at) {
            float sum = xyz[at] + xyz[at + 1] + xyz[at + 2];
            return new float[] { xyz[at] / sum, xyz[at + 1] / sum };
        }
        private static void checkMatrix(float[] toXYZ) {
            if (toXYZ == null || toXYZ.length != 9) throw new IllegalArgumentException("The transform of a color space must be an array of 9 floats");
        }
        private static boolean isGray(float[] m) { return m != null && m.length == 9 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[5] == 0 && m[6] == 0 && m[7] == 0; }
        /** The primaries a transform to XYZ has: where it takes red, green and blue. */
        private static float[] primariesOf(float[] toXYZ) {
            checkMatrix(toXYZ);
            float[] r = xy(toXYZ, 0), g = xy(toXYZ, 3), b = xy(toXYZ, 6);
            return new float[] { r[0], r[1], g[0], g[1], b[0], b[1] };
        }
        /** And its white point: where it takes white. */
        private static float[] whitePointOf(float[] toXYZ) {
            checkMatrix(toXYZ);
            return xy(mulVec(toXYZ, new float[] { 1, 1, 1 }));
        }
        /** The transform to XYZ of three primaries and a white point: each primary's XYZ of luminance 1 is a column, and
         *  the columns are scaled so that red, green and blue together are the white. */
        private static float[] toXyzOf(float[] p, float[] w) {
            float[] columns = new float[9];
            for (int i = 0; i < 3; i++) {
                float x = p[i * 2], y = p[i * 2 + 1];
                columns[i * 3] = x / y; columns[i * 3 + 1] = 1; columns[i * 3 + 2] = (1 - x - y) / y;
            }
            float[] scale = mulVec(inverse(columns), xyToXyz(w));
            for (int i = 0; i < 9; i++) columns[i] *= scale[i / 3];
            return columns;
        }
        private static float area(float[] p) {
            float a = 0.5f * (p[0] * p[3] + p[1] * p[4] + p[2] * p[5] - p[3] * p[4] - p[1] * p[2] - p[0] * p[5]);
            return a < 0 ? -a : a;
        }
        private static float cross(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }
        /** Whether the triangle of the first primaries holds that of the second, as Android decides it: each primary of the
         *  first lies in the angle that opens outward from the matching primary of the second. */
        private static boolean holds(float[] outer, float[] inner) {
            for (int i = 0; i < 3; i++) {
                int prev = (i + 2) % 3, next = (i + 1) % 3;
                float dx = outer[i * 2] - inner[i * 2], dy = outer[i * 2 + 1] - inner[i * 2 + 1];
                if (cross(dx, dy, inner[i * 2] - inner[prev * 2], inner[i * 2 + 1] - inner[prev * 2 + 1]) < 0) return false;
                if (cross(inner[i * 2] - inner[next * 2], inner[i * 2 + 1] - inner[next * 2 + 1], dx, dy) < 0) return false;
            }
            return true;
        }

        /** A coded value as light, by the parameters. */
        static double decode(TransferParameters p, double x) { return x >= p.d ? Math.pow(p.a * x + p.b, p.g) + p.e : p.c * x + p.f; }
        /** Light as a coded value: the same function backwards. */
        static double encode(TransferParameters p, double x) { return x >= p.d * p.c ? (Math.pow(x - p.e, 1.0 / p.g) - p.b) / p.a : (x - p.f) / p.c; }
        /** Hybrid log-gamma: a square below the knee and an exponential above it, times the scale f + 1. */
        private static double decodeHlg(TransferParameters p, double x) {
            double sign = x < 0 ? -1.0 : 1.0, v = x * sign;
            return (p.f + 1.0) * sign * (v * p.a <= 1 ? Math.pow(v * p.a, p.b) : Math.exp((v - p.e) * p.c) + p.d);
        }
        private static double encodeHlg(TransferParameters p, double x) {
            double sign = x < 0 ? -1.0 : 1.0, v = x * sign / (p.f + 1.0);
            return sign * (v <= 1 ? Math.pow(v, 1.0 / p.b) / p.a : Math.log(v - p.d) / p.c + p.e);
        }
        /** The perceptual quantizer: (max(a + b x^c, 0) / (d + e x^c))^f. */
        private static double decodePq(TransferParameters p, double x) {
            double sign = x < 0 ? -1.0 : 1.0, v = Math.pow(x * sign, p.c);
            return sign * Math.pow(Math.max(p.a + p.b * v, 0) / (p.d + p.e * v), p.f);
        }
        private static double encodePq(TransferParameters p, double x) {
            double sign = x < 0 ? -1.0 : 1.0, v = Math.pow(x * sign, 1.0 / p.f);
            return sign * Math.pow(Math.max(-p.a + p.d * v, 0) / (p.b - p.e * v), 1.0 / p.c);
        }
        private static DoubleUnaryOperator eotfOf(final TransferParameters p) {
            if (p == null) throw new IllegalArgumentException("The transfer functions of a color space cannot be null");
            if (p.g == TransferParameters.TYPE_HLGISH) return x -> decodeHlg(p, x);
            if (p.g == TransferParameters.TYPE_PQISH) return x -> decodePq(p, x);
            return x -> decode(p, x);
        }
        private static DoubleUnaryOperator oetfOf(final TransferParameters p) {
            if (p == null) throw new IllegalArgumentException("The transfer functions of a color space cannot be null");
            if (p.g == TransferParameters.TYPE_HLGISH) return x -> encodeHlg(p, x);
            if (p.g == TransferParameters.TYPE_PQISH) return x -> encodePq(p, x);
            return x -> encode(p, x);
        }

        public float[] getWhitePoint() { return Arrays.copyOf(mWhitePoint, 2); }
        public float[] getWhitePoint(float[] whitePoint) { System.arraycopy(mWhitePoint, 0, whitePoint, 0, 2); return whitePoint; }
        public float[] getPrimaries() { return Arrays.copyOf(mPrimaries, 6); }
        public float[] getPrimaries(float[] primaries) { System.arraycopy(mPrimaries, 0, primaries, 0, 6); return primaries; }
        public float[] getTransform() { return Arrays.copyOf(mTransform, 9); }
        public float[] getTransform(float[] transform) { System.arraycopy(mTransform, 0, transform, 0, 9); return transform; }
        public float[] getInverseTransform() { return Arrays.copyOf(mInverseTransform, 9); }
        public float[] getInverseTransform(float[] inverseTransform) { System.arraycopy(mInverseTransform, 0, inverseTransform, 0, 9); return inverseTransform; }
        /** Light to coded values, and back. */
        public DoubleUnaryOperator getOetf() { return mOetf; }
        public DoubleUnaryOperator getEotf() { return mEotf; }
        /** The parameters of the transfer function, or null for a space made of two functions. */
        public TransferParameters getTransferParameters() { return mTransferParameters; }
        @Override public boolean isSrgb() { return mIsSrgb; }
        @Override public boolean isWideGamut() { return mIsWideGamut; }
        @Override public float getMinValue(int component) { return mMin; }
        @Override public float getMaxValue(int component) { return mMax; }
        public float[] toLinear(float r, float g, float b) { return toLinear(new float[] { r, g, b }); }
        public float[] toLinear(float[] v) { for (int i = 0; i < 3; i++) v[i] = (float) mClampedEotf.applyAsDouble(v[i]); return v; }
        public float[] fromLinear(float r, float g, float b) { return fromLinear(new float[] { r, g, b }); }
        public float[] fromLinear(float[] v) { for (int i = 0; i < 3; i++) v[i] = (float) mClampedOetf.applyAsDouble(v[i]); return v; }
        @Override public float[] toXyz(float[] v) { return mulVec(mTransform, toLinear(v)); }
        @Override public float[] fromXyz(float[] v) { return fromLinear(mulVec(mInverseTransform, v)); }
        @Override public boolean equals(Object o) {
            if (this == o) return true;
            if (!super.equals(o)) return false;
            Rgb rgb = (Rgb) o;
            if (Float.compare(rgb.mMin, mMin) != 0 || Float.compare(rgb.mMax, mMax) != 0) return false;
            if (!Arrays.equals(mWhitePoint, rgb.mWhitePoint) || !Arrays.equals(mPrimaries, rgb.mPrimaries)) return false;
            if (mTransferParameters != null) return mTransferParameters.equals(rgb.mTransferParameters);
            return rgb.mTransferParameters == null && mOetf.equals(rgb.mOetf) && mEotf.equals(rgb.mEotf);
        }
        @Override public int hashCode() {
            int h = super.hashCode();
            h = 31 * h + Arrays.hashCode(mWhitePoint);
            h = 31 * h + Arrays.hashCode(mPrimaries);
            h = 31 * h + Float.floatToIntBits(mMin);
            h = 31 * h + Float.floatToIntBits(mMax);
            return 31 * h + (mTransferParameters != null ? mTransferParameters.hashCode() : mOetf.hashCode() * 31 + mEotf.hashCode());
        }
    }

    /** CIE XYZ itself, of the white point D50. */
    private static final class Xyz extends ColorSpace {
        Xyz(String name, int id) { super(name, Model.XYZ, id); }
        @Override public boolean isWideGamut() { return true; }
        @Override public float getMinValue(int component) { return -2.0f; }
        @Override public float getMaxValue(int component) { return 2.0f; }
        private static float[] clamp(float[] v) { for (int i = 0; i < 3; i++) v[i] = v[i] < -2.0f ? -2.0f : v[i] > 2.0f ? 2.0f : v[i]; return v; }
        @Override public float[] toXyz(float[] v) { return clamp(v); }
        @Override public float[] fromXyz(float[] v) { return clamp(v); }
    }

    /** CIE L*a*b* of the white point D50. */
    private static final class Lab extends ColorSpace {
        private static final float A = 216.0f / 24389.0f, B = 841.0f / 108.0f, C = 4.0f / 29.0f, D = 6.0f / 29.0f;
        Lab(String name, int id) { super(name, Model.LAB, id); }
        @Override public boolean isWideGamut() { return true; }
        @Override public float getMinValue(int component) { return component == 0 ? 0.0f : -128.0f; }
        @Override public float getMaxValue(int component) { return component == 0 ? 100.0f : 128.0f; }
        private static float clamp(float x, float min, float max) { return x < min ? min : x > max ? max : x; }
        private static float cube(float f) { return f > D ? f * f * f : (f - C) / B; }
        private static float root(float t) { return t > A ? (float) Math.cbrt(t) : B * t + C; }
        @Override public float[] toXyz(float[] v) {
            float l = clamp(v[0], 0.0f, 100.0f), a = clamp(v[1], -128.0f, 128.0f), b = clamp(v[2], -128.0f, 128.0f);
            float fy = (l + 16.0f) / 116.0f, fx = fy + a * 0.002f, fz = fy - b * 0.005f;
            v[0] = cube(fx) * D50_XYZ[0];
            v[1] = cube(fy) * D50_XYZ[1];
            v[2] = cube(fz) * D50_XYZ[2];
            return v;
        }
        @Override public float[] fromXyz(float[] v) {
            float fx = root(v[0] / D50_XYZ[0]), fy = root(v[1] / D50_XYZ[1]), fz = root(v[2] / D50_XYZ[2]);
            v[0] = clamp(116.0f * fy - 16.0f, 0.0f, 100.0f);
            v[1] = clamp(500.0f * (fx - fy), -128.0f, 128.0f);
            v[2] = clamp(200.0f * (fy - fz), -128.0f, 128.0f);
            return v;
        }
    }

    /** From one space to another, through XYZ of D50: an RGB space of another white point is adapted to it first. */
    public static class Connector {
        private final ColorSpace mSource, mDestination, mFrom, mTo;
        private final RenderIntent mIntent;
        /** With the absolute intent and one RGB space, the scale between that space's white and D50; else null. */
        private final float[] mScale;

        Connector(ColorSpace source, ColorSpace destination, RenderIntent intent) {
            this(source, destination, source.getModel() == Model.RGB ? adapt(source, D50_XYZ) : source,
                destination.getModel() == Model.RGB ? adapt(destination, D50_XYZ) : destination, intent, scaleOf(source, destination, intent));
        }
        Connector(ColorSpace source, ColorSpace destination, ColorSpace from, ColorSpace to, RenderIntent intent, float[] scale) {
            mSource = source; mDestination = destination; mFrom = from; mTo = to; mIntent = intent; mScale = scale;
        }
        private static float[] scaleOf(ColorSpace source, ColorSpace destination, RenderIntent intent) {
            boolean srcRgb = source.getModel() == Model.RGB, dstRgb = destination.getModel() == Model.RGB;
            if (intent != RenderIntent.ABSOLUTE || srcRgb == dstRgb) return null;
            Rgb rgb = (Rgb) (srcRgb ? source : destination);
            float[] src = srcRgb ? xyToXyz(rgb.mWhitePoint) : D50_XYZ, dst = dstRgb ? xyToXyz(rgb.mWhitePoint) : D50_XYZ;
            return new float[] { src[0] / dst[0], src[1] / dst[1], src[2] / dst[2] };
        }
        static Connector identity(ColorSpace space) {
            return new Connector(space, space, space, space, RenderIntent.RELATIVE, null) {
                @Override public float[] transform(float[] v) { return v; }
            };
        }
        public ColorSpace getSource() { return mSource; }
        public ColorSpace getDestination() { return mDestination; }
        public RenderIntent getRenderIntent() { return mIntent; }
        public float[] transform(float r, float g, float b) { return transform(new float[] { r, g, b }); }
        /** The colour in the destination, in the array it came in. */
        public float[] transform(float[] v) {
            float[] xyz = mFrom.toXyz(v);
            if (mScale != null) { xyz[0] *= mScale[0]; xyz[1] *= mScale[1]; xyz[2] *= mScale[2]; }
            return mTo.fromXyz(xyz);
        }
    }

    /** Between two RGB spaces: one matrix between the two transfer functions. */
    private static final class RgbConnector extends Connector {
        private final Rgb mFromRgb, mToRgb;
        private final float[] mMatrix;
        RgbConnector(Rgb source, Rgb destination, RenderIntent intent) {
            super(source, destination, source, destination, intent, null);
            mFromRgb = source;
            mToRgb = destination;
            if (near(source.mWhitePoint, destination.mWhitePoint)) {
                // One white point: straight through the XYZ they share.
                mMatrix = mul(destination.mInverseTransform, source.mTransform);
            } else {
                float[] forward = source.mTransform, back = destination.mInverseTransform;
                float[] srcWhite = xyToXyz(source.mWhitePoint), dstWhite = xyToXyz(destination.mWhitePoint);
                if (!near(source.mWhitePoint, ILLUMINANT_D50)) forward = mul(adaptation(Adaptation.BRADFORD.mTransform, srcWhite, D50_XYZ), source.mTransform);
                if (!near(destination.mWhitePoint, ILLUMINANT_D50)) back = inverse(mul(adaptation(Adaptation.BRADFORD.mTransform, dstWhite, D50_XYZ), destination.mTransform));
                if (intent == RenderIntent.ABSOLUTE) forward = mulDiag(new float[] { srcWhite[0] / dstWhite[0], srcWhite[1] / dstWhite[1], srcWhite[2] / dstWhite[2] }, forward);
                mMatrix = mul(back, forward);
            }
        }
        @Override public float[] transform(float[] v) { return mToRgb.fromLinear(mulVec(mMatrix, mFromRgb.toLinear(v))); }
    }
}
