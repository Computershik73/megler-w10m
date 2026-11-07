// Overlay of core/framework/src/android/graphics/Matrix.java (tools/build-framework.sh).
// It differs in making no garbage: every concatenation (mul) made a new float[9] for its result, and every pre- and post-
// translate, scale, rotate and skew a float[9] for the operand as well; invert, mapRect, setRectToRect, setPolyToPoly and
// setSinCos one each. Now each works in local variables and writes the matrix's own array: pre and post operations of an
// affine operand go through preAffine and postAffine, a concatenation of two matrices through mul, which reads all of both
// before it writes (a matrix may be concatenated with itself).
//
// Why. Spotify asks where its views are, to log what is seen, all the time: View.getLocationInWindow,
// getGlobalVisibleRect and Compose's transformMatrixToGlobal, every one of them through these. Its main thread made 6 MB of
// these arrays a second, half of all it made, and the collector ran every second and stopped it for 200 ms; once it spent
// 34 s on input and messages and drew one frame (package 663). Everything else is the original's, unchanged.
package android.graphics;

/** A 3x3 transform, as Android keeps it: MSCALE_X, MSKEW_X, MTRANS_X / MSKEW_Y, MSCALE_Y, MTRANS_Y / MPERSP_0..2. */
public class Matrix {
    public static final int MSCALE_X = 0, MSKEW_X = 1, MTRANS_X = 2, MSKEW_Y = 3, MSCALE_Y = 4, MTRANS_Y = 5, MPERSP_0 = 6, MPERSP_1 = 7, MPERSP_2 = 8;
    public enum ScaleToFit { FILL, START, CENTER, END }
    public static final Matrix IDENTITY_MATRIX = new Matrix();

    final float[] m = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    public Matrix() {}
    public Matrix(Matrix src) { if (src != null) set(src); }

    public boolean isIdentity() { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 1 && m[5] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 1; }
    public boolean isAffine() { return m[6] == 0 && m[7] == 0 && m[8] == 1; }
    public boolean rectStaysRect() { return (m[1] == 0 && m[3] == 0) || (m[0] == 0 && m[4] == 0); }
    public void set(Matrix src) { if (src == null) reset(); else System.arraycopy(src.m, 0, m, 0, 9); }
    public void reset() { m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 1; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = 1; }
    public void getValues(float[] v) { System.arraycopy(m, 0, v, 0, 9); }
    public void setValues(float[] v) { System.arraycopy(v, 0, m, 0, 9); }
    /** The affine part, as megler.Gfx takes it. */
    public float[] meglerAffine() { return new float[] { m[0], m[1], m[2], m[3], m[4], m[5] }; }

    /* out = a * b, every element of both read before out is written: out may be either. */
    private static void mul(float[] out, float[] a, float[] b) {
        float r0 = a[0] * b[0] + a[1] * b[3] + a[2] * b[6], r1 = a[0] * b[1] + a[1] * b[4] + a[2] * b[7], r2 = a[0] * b[2] + a[1] * b[5] + a[2] * b[8];
        float r3 = a[3] * b[0] + a[4] * b[3] + a[5] * b[6], r4 = a[3] * b[1] + a[4] * b[4] + a[5] * b[7], r5 = a[3] * b[2] + a[4] * b[5] + a[5] * b[8];
        float r6 = a[6] * b[0] + a[7] * b[3] + a[8] * b[6], r7 = a[6] * b[1] + a[7] * b[4] + a[8] * b[7], r8 = a[6] * b[2] + a[7] * b[5] + a[8] * b[8];
        out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4; out[5] = r5; out[6] = r6; out[7] = r7; out[8] = r8;
    }
    /* m = m * B, B the affine matrix (a b c / d e f / 0 0 1). */
    private void preAffine(float a, float b, float c, float d, float e, float f) {
        float m0 = m[0], m1 = m[1], m3 = m[3], m4 = m[4], m6 = m[6], m7 = m[7];
        m[0] = m0 * a + m1 * d; m[1] = m0 * b + m1 * e; m[2] += m0 * c + m1 * f;
        m[3] = m3 * a + m4 * d; m[4] = m3 * b + m4 * e; m[5] += m3 * c + m4 * f;
        m[6] = m6 * a + m7 * d; m[7] = m6 * b + m7 * e; m[8] += m6 * c + m7 * f;
    }
    /* m = B * m, B the affine matrix (a b c / d e f / 0 0 1). */
    private void postAffine(float a, float b, float c, float d, float e, float f) {
        float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7], m8 = m[8];
        m[0] = a * m0 + b * m3 + c * m6; m[1] = a * m1 + b * m4 + c * m7; m[2] = a * m2 + b * m5 + c * m8;
        m[3] = d * m0 + e * m3 + f * m6; m[4] = d * m1 + e * m4 + f * m7; m[5] = d * m2 + e * m5 + f * m8;
    }
    private void setAffine(float a, float b, float c, float d, float e, float f) {
        m[0] = a; m[1] = b; m[2] = c; m[3] = d; m[4] = e; m[5] = f; m[6] = 0; m[7] = 0; m[8] = 1;
    }

    public void setTranslate(float dx, float dy) { setAffine(1, 0, dx, 0, 1, dy); }
    public void setScale(float sx, float sy, float px, float py) { setAffine(sx, 0, px - sx * px, 0, sy, py - sy * py); }
    public void setScale(float sx, float sy) { setScale(sx, sy, 0, 0); }
    public void setRotate(float d, float px, float py) {
        double a = Math.toRadians(d);
        setSinCos((float) Math.sin(a), (float) Math.cos(a), px, py);
    }
    public void setRotate(float d) { setRotate(d, 0, 0); }
    public void setSinCos(float sin, float cos, float px, float py) { setAffine(cos, -sin, px - cos * px + sin * py, sin, cos, py - sin * px - cos * py); }
    public void setSinCos(float sin, float cos) { setSinCos(sin, cos, 0, 0); }
    public void setSkew(float kx, float ky, float px, float py) { setAffine(1, kx, -kx * py, ky, 1, -ky * px); }
    public void setSkew(float kx, float ky) { setSkew(kx, ky, 0, 0); }
    public boolean setConcat(Matrix a, Matrix b) { mul(m, a.m, b.m); return true; }
    public boolean preTranslate(float dx, float dy) { preAffine(1, 0, dx, 0, 1, dy); return true; }
    public boolean preScale(float sx, float sy, float px, float py) { preAffine(sx, 0, px - sx * px, 0, sy, py - sy * py); return true; }
    public boolean preScale(float sx, float sy) { return preScale(sx, sy, 0, 0); }
    public boolean preRotate(float d, float px, float py) {
        double a = Math.toRadians(d);
        float c = (float) Math.cos(a), sn = (float) Math.sin(a);
        preAffine(c, -sn, px - c * px + sn * py, sn, c, py - sn * px - c * py);
        return true;
    }
    public boolean preRotate(float d) { return preRotate(d, 0, 0); }
    public boolean preSkew(float kx, float ky, float px, float py) { preAffine(1, kx, -kx * py, ky, 1, -ky * px); return true; }
    public boolean preSkew(float kx, float ky) { return preSkew(kx, ky, 0, 0); }
    public boolean preConcat(Matrix o) { mul(m, m, o.m); return true; }
    public boolean postTranslate(float dx, float dy) { postAffine(1, 0, dx, 0, 1, dy); return true; }
    public boolean postScale(float sx, float sy, float px, float py) { postAffine(sx, 0, px - sx * px, 0, sy, py - sy * py); return true; }
    public boolean postScale(float sx, float sy) { return postScale(sx, sy, 0, 0); }
    public boolean postRotate(float d, float px, float py) {
        double a = Math.toRadians(d);
        float c = (float) Math.cos(a), sn = (float) Math.sin(a);
        postAffine(c, -sn, px - c * px + sn * py, sn, c, py - sn * px - c * py);
        return true;
    }
    public boolean postRotate(float d) { return postRotate(d, 0, 0); }
    public boolean postSkew(float kx, float ky, float px, float py) { postAffine(1, kx, -kx * py, ky, 1, -ky * px); return true; }
    public boolean postSkew(float kx, float ky) { return postSkew(kx, ky, 0, 0); }
    public boolean postConcat(Matrix o) { mul(m, o.m, m); return true; }

    public boolean invert(Matrix inverse) {
        float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
        float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        float det = a * A + b * B + c * C;
        if (det == 0 || Float.isNaN(det)) return false;
        if (inverse != null) {
            float[] o = inverse.m;
            o[0] = A / det; o[1] = -(b * i - c * h) / det; o[2] = (b * f - c * e) / det;
            o[3] = B / det; o[4] = (a * i - c * g) / det; o[5] = -(a * f - c * d) / det;
            o[6] = C / det; o[7] = -(a * h - b * g) / det; o[8] = (a * e - b * d) / det;
        }
        return true;
    }
    public void mapPoints(float[] dst, int dstIndex, float[] src, int srcIndex, int pointCount) {
        for (int p = 0; p < pointCount; p++) {
            float x = src[srcIndex + 2 * p], y = src[srcIndex + 2 * p + 1];
            float w = m[6] * x + m[7] * y + m[8]; if (w == 0) w = 1;
            dst[dstIndex + 2 * p] = (m[0] * x + m[1] * y + m[2]) / w;
            dst[dstIndex + 2 * p + 1] = (m[3] * x + m[4] * y + m[5]) / w;
        }
    }
    public void mapPoints(float[] dst, float[] src) { mapPoints(dst, 0, src, 0, src.length / 2); }
    public void mapPoints(float[] pts) { mapPoints(pts, 0, pts, 0, pts.length / 2); }
    public void mapVectors(float[] dst, int dstIndex, float[] src, int srcIndex, int n) {
        for (int p = 0; p < n; p++) {
            float x = src[srcIndex + 2 * p], y = src[srcIndex + 2 * p + 1];
            dst[dstIndex + 2 * p] = m[0] * x + m[1] * y; dst[dstIndex + 2 * p + 1] = m[3] * x + m[4] * y;
        }
    }
    public void mapVectors(float[] v) { mapVectors(v, 0, v, 0, v.length / 2); }
    public void mapVectors(float[] dst, float[] src) { mapVectors(dst, 0, src, 0, src.length / 2); }
    public boolean mapRect(RectF dst, RectF src) {
        float sl = src.left, st = src.top, sr = src.right, sb = src.bottom;
        float x0 = mapX(sl, st), y0 = mapY(sl, st), x1 = mapX(sr, st), y1 = mapY(sr, st);
        float x2 = mapX(sr, sb), y2 = mapY(sr, sb), x3 = mapX(sl, sb), y3 = mapY(sl, sb);
        dst.set(Math.min(Math.min(x0, x1), Math.min(x2, x3)), Math.min(Math.min(y0, y1), Math.min(y2, y3)),
                Math.max(Math.max(x0, x1), Math.max(x2, x3)), Math.max(Math.max(y0, y1), Math.max(y2, y3)));
        return rectStaysRect();
    }
    /* A point's x and y through the matrix, as mapPoints maps it. */
    private float mapX(float x, float y) { float w = m[6] * x + m[7] * y + m[8]; if (w == 0) w = 1; return (m[0] * x + m[1] * y + m[2]) / w; }
    private float mapY(float x, float y) { float w = m[6] * x + m[7] * y + m[8]; if (w == 0) w = 1; return (m[3] * x + m[4] * y + m[5]) / w; }
    public boolean mapRect(RectF r) { return mapRect(r, r); }
    public float mapRadius(float radius) { float[] v = { radius, 0, 0, radius }; mapVectors(v); return (float) Math.sqrt(Math.hypot(v[0], v[1]) * Math.hypot(v[2], v[3])); }
    public boolean setRectToRect(RectF src, RectF dst, ScaleToFit stf) {
        if (src.isEmpty()) { reset(); return false; }
        float sx = dst.width() / src.width(), sy = dst.height() / src.height();
        float tx, ty;
        if (stf == ScaleToFit.FILL) { tx = dst.left - src.left * sx; ty = dst.top - src.top * sy; }
        else {
            float sc = Math.min(sx, sy); sx = sy = sc;
            float dw = src.width() * sc, dh = src.height() * sc;
            float ox = 0, oy = 0;
            if (stf == ScaleToFit.CENTER) { ox = (dst.width() - dw) / 2; oy = (dst.height() - dh) / 2; }
            else if (stf == ScaleToFit.END) { ox = dst.width() - dw; oy = dst.height() - dh; }
            tx = dst.left + ox - src.left * sc; ty = dst.top + oy - src.top * sc;
        }
        setAffine(sx, 0, tx, 0, sy, ty);
        return true;
    }
    public boolean setPolyToPoly(float[] src, int srcIndex, float[] dst, int dstIndex, int pointCount) {
        if (pointCount == 0) { reset(); return true; }
        if (pointCount == 1) { setTranslate(dst[dstIndex] - src[srcIndex], dst[dstIndex + 1] - src[srcIndex + 1]); return true; }
        // two or more points: a similarity from the first two (enough for the uses seen)
        float sx0 = src[srcIndex], sy0 = src[srcIndex + 1], sx1 = src[srcIndex + 2], sy1 = src[srcIndex + 3];
        float dx0 = dst[dstIndex], dy0 = dst[dstIndex + 1], dx1 = dst[dstIndex + 2], dy1 = dst[dstIndex + 3];
        float svx = sx1 - sx0, svy = sy1 - sy0, dvx = dx1 - dx0, dvy = dy1 - dy0;
        float sl = svx * svx + svy * svy; if (sl == 0) return false;
        float a = (svx * dvx + svy * dvy) / sl, b = (svx * dvy - svy * dvx) / sl;
        setAffine(a, -b, dx0 - (a * sx0 - b * sy0), b, a, dy0 - (b * sx0 + a * sy0));
        return true;
    }
    @Override public boolean equals(Object o) { return o instanceof Matrix && java.util.Arrays.equals(m, ((Matrix) o).m); }
    @Override public int hashCode() { return java.util.Arrays.hashCode(m); }
    @Override public String toString() { return "Matrix" + java.util.Arrays.toString(m); }
    public String toShortString() { return java.util.Arrays.toString(m); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public void dump(java.io.PrintWriter p0) {}
    public long ni() { return 0L; }
    // ---- end of generated members
}
