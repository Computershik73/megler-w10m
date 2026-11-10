// Overlay of core/framework/src/android/graphics/Path.java (tools/build-framework.sh).
// It differs in the inverse fill types: toggleInverseFillType did nothing, and meglerFill, what Canvas hands megler.Gfx with
// the path, said only winding or even-odd, so a path set to INVERSE_WINDING or INVERSE_EVEN_ODD was drawn and clipped
// by as if it were not inverse. Now toggleInverseFillType turns WINDING into INVERSE_WINDING and back, EVEN_ODD into
// INVERSE_EVEN_ODD and back, as Android's does, and meglerFill has a second bit for inverse (0 winding, 1 even-odd, 2
// inverse winding, 3 inverse even-odd), which core/src/drivers/mg_java_gfx.c draws and clips by.
//
// Why. An inverse path is how a view rounds its corners by painting over them: VK mp3 mod's BarcodeViewfinderLayout, the
// frame round its QR scanner's camera picture, toggles a rounded rectangle to inverse in its constructor and fills it in
// white after its children have drawn -- the four corners outside the rounded rectangle. Taken as an ordinary path, the
// white filled the rectangle itself, over the whole camera picture: the scanner showed white and nothing else on the
// phone, while the camera streamed and every picture was drawn under it.
//
// And in getPathIterator, which gave null: now an iterator over the segments megler.Gfx draws (PathIterator's overlay),
// which Compose walks paths with on Android 14 and later. And in op, which did UNION alone and left the path as it was for
// the rest: DIFFERENCE, REVERSE_DIFFERENCE, XOR and INTERSECT as the even-odd rule does them (see op). And meglerSetOutlines,
// for Paint.getTextPath: the path made of the glyphs' outlines alone. Everything else is the original's, unchanged.
package android.graphics;

/** A path, kept as the op stream megler.Gfx draws (see core/src/drivers/mg_java_gfx.c: OP_*). */
public class Path {
    public enum FillType { WINDING, EVEN_ODD, INVERSE_WINDING, INVERSE_EVEN_ODD }
    public enum Direction { CW, CCW }
    public enum Op { DIFFERENCE, INTERSECT, UNION, XOR, REVERSE_DIFFERENCE }
    static final int MOVE = 0, LINE = 1, QUAD = 2, CUBIC = 3, CLOSE = 4, ARC = 5, RECT = 6, OVAL = 7, RRECT = 8, TRANSFORM = 9,
        RMOVE = 10, RLINE = 11, RQUAD = 12, RCUBIC = 13, RRECT8 = 14;

    float[] ops = new float[32];
    int n;
    private FillType fill = FillType.WINDING;
    private float lastX, lastY;
    private boolean any;

    public Path() {}
    public Path(Path src) { set(src); }

    private void put(int op, float... v) {
        if (n + 1 + v.length > ops.length) ops = java.util.Arrays.copyOf(ops, Math.max(ops.length * 2, n + 1 + v.length + 16));
        ops[n++] = op;
        for (float f : v) ops[n++] = f;
        any = true;
    }
    public void set(Path src) { if (src == this) return; ops = src.ops.clone(); n = src.n; fill = src.fill; lastX = src.lastX; lastY = src.lastY; any = src.any; }
    public void reset() { n = 0; any = false; lastX = lastY = 0; }
    public void rewind() { reset(); }
    public boolean isEmpty() { return !any; }
    public boolean isConvex() { return false; }
    public boolean isRect(RectF r) { return false; }
    public boolean isInverseFillType() { return fill == FillType.INVERSE_WINDING || fill == FillType.INVERSE_EVEN_ODD; }
    public void toggleInverseFillType() {
        switch (fill) {
        case WINDING: fill = FillType.INVERSE_WINDING; break;
        case EVEN_ODD: fill = FillType.INVERSE_EVEN_ODD; break;
        case INVERSE_WINDING: fill = FillType.WINDING; break;
        default: fill = FillType.EVEN_ODD; break;
        }
    }
    public FillType getFillType() { return fill; }
    public void setFillType(FillType f) { fill = f; }
    public void incReserve(int extra) {}
    public void moveTo(float x, float y) { put(MOVE, x, y); lastX = x; lastY = y; }
    public void rMoveTo(float dx, float dy) { put(RMOVE, dx, dy); lastX += dx; lastY += dy; }
    public void lineTo(float x, float y) { put(LINE, x, y); lastX = x; lastY = y; }
    public void rLineTo(float dx, float dy) { put(RLINE, dx, dy); lastX += dx; lastY += dy; }
    public void quadTo(float x1, float y1, float x2, float y2) { put(QUAD, x1, y1, x2, y2); lastX = x2; lastY = y2; }
    public void rQuadTo(float dx1, float dy1, float dx2, float dy2) { put(RQUAD, dx1, dy1, dx2, dy2); lastX += dx2; lastY += dy2; }
    public void conicTo(float x1, float y1, float x2, float y2, float w) { quadTo(x1, y1, x2, y2); }
    public void rConicTo(float dx1, float dy1, float dx2, float dy2, float w) { rQuadTo(dx1, dy1, dx2, dy2); }
    public void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) { put(CUBIC, x1, y1, x2, y2, x3, y3); lastX = x3; lastY = y3; }
    public void rCubicTo(float x1, float y1, float x2, float y2, float x3, float y3) { put(RCUBIC, x1, y1, x2, y2, x3, y3); lastX += x3; lastY += y3; }
    public void arcTo(RectF o, float start, float sweep, boolean forceMoveTo) { arcTo(o.left, o.top, o.right, o.bottom, start, sweep, forceMoveTo); }
    public void arcTo(RectF o, float start, float sweep) { arcTo(o, start, sweep, false); }
    public void arcTo(float l, float t, float r, float b, float start, float sweep, boolean forceMoveTo) {
        put(ARC, l, t, r, b, start, sweep, forceMoveTo ? 1 : 0);
        double a = Math.toRadians(start + sweep);
        lastX = (float) ((l + r) / 2 + (r - l) / 2 * Math.cos(a)); lastY = (float) ((t + b) / 2 + (b - t) / 2 * Math.sin(a));
    }
    public void close() { put(CLOSE); }
    public void addRect(RectF r, Direction d) { addRect(r.left, r.top, r.right, r.bottom, d); }
    public void addRect(float l, float t, float r, float b, Direction d) { put(RECT, l, t, r, b, d == Direction.CCW ? 1 : 0); }
    public void addOval(RectF o, Direction d) { addOval(o.left, o.top, o.right, o.bottom, d); }
    public void addOval(float l, float t, float r, float b, Direction d) { put(OVAL, l, t, r, b, d == Direction.CCW ? 1 : 0); }
    public void addCircle(float x, float y, float radius, Direction d) { addOval(x - radius, y - radius, x + radius, y + radius, d); }
    public void addArc(RectF o, float start, float sweep) { addArc(o.left, o.top, o.right, o.bottom, start, sweep); }
    public void addArc(float l, float t, float r, float b, float start, float sweep) { put(ARC, l, t, r, b, start, sweep, 1); }
    public void addRoundRect(RectF r, float rx, float ry, Direction d) { addRoundRect(r.left, r.top, r.right, r.bottom, rx, ry, d); }
    public void addRoundRect(float l, float t, float r, float b, float rx, float ry, Direction d) { put(RRECT, l, t, r, b, rx, ry, d == Direction.CCW ? 1 : 0); }
    public void addRoundRect(RectF r, float[] radii, Direction d) { addRoundRect(r.left, r.top, r.right, r.bottom, radii, d); }
    public void addRoundRect(float l, float t, float r, float b, float[] radii, Direction d) {
        put(RRECT8, l, t, r, b, radii[0], radii[1], radii[2], radii[3], radii[4], radii[5], radii[6], radii[7], d == Direction.CCW ? 1 : 0);
    }
    public void addPath(Path src) { if (src.n == 0) return; float[] o = java.util.Arrays.copyOf(src.ops, src.n); appendRaw(o); }
    public void addPath(Path src, float dx, float dy) { Path p = new Path(src); p.offset(dx, dy); addPath(p); }
    public void addPath(Path src, Matrix m) { Path p = new Path(src); p.transform(m); addPath(p); }
    private void appendRaw(float[] o) {
        if (n + o.length > ops.length) ops = java.util.Arrays.copyOf(ops, n + o.length + 16);
        System.arraycopy(o, 0, ops, n, o.length); n += o.length; any = true;
    }
    /** For Paint.getTextPath: this path made of the glyphs' outlines alone (megler.Gfx.txPath's stream, this class's own verbs), filled
        by WINDING, the last point where the stream leaves it. */
    void meglerSetOutlines(float[] o) {
        reset();
        fill = FillType.WINDING;
        if (o.length == 0) return;
        appendRaw(o);
        float sx = 0, sy = 0;
        for (int i = 0; i < o.length;) {
            int verb = (int) o[i++], count = verb == QUAD ? 4 : verb == CUBIC ? 6 : verb == CLOSE ? 0 : 2;
            if (verb == CLOSE) { lastX = sx; lastY = sy; continue; }
            lastX = o[i + count - 2]; lastY = o[i + count - 1];
            if (verb == MOVE) { sx = lastX; sy = lastY; }
            i += count;
        }
    }
    public void offset(float dx, float dy) { transform(translate(dx, dy)); }
    public void offset(float dx, float dy, Path dst) { if (dst != null) { dst.set(this); dst.offset(dx, dy); } else offset(dx, dy); }
    private static Matrix translate(float dx, float dy) { Matrix m = new Matrix(); m.setTranslate(dx, dy); return m; }
    public void transform(Matrix m) { if (m == null || m.isIdentity() || n == 0) return; put(TRANSFORM, m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5]); float[] p = { lastX, lastY }; m.mapPoints(p); lastX = p[0]; lastY = p[1]; }
    public void transform(Matrix m, Path dst) { if (dst != null) { dst.set(this); dst.transform(m); } else transform(m); }
    public void setLastPoint(float x, float y) { lastX = x; lastY = y; }
    public void computeBounds(RectF bounds, boolean exact) {
        if (n == 0) { bounds.set(0, 0, 0, 0); return; }
        float[] o = new float[4];
        megler.Gfx.pathBounds(ops, n, o);
        bounds.set(o[0], o[1], o[2], o[3]);
    }
    /*
     * Boolean operations, done with what megler.Gfx fills: a shape within a rectangle taken out of it is the two with the
     * even-odd rule -- TikTok Lite rounds its frames' corners so, the frame's rectangle less its rounded rectangle painted
     * with DST_OUT over the content, and op, which answered false and left the whole rectangle, took the whole content
     * away: its consent dialog and its feed showed nothing (package 624). Exactly: DIFFERENCE and REVERSE_DIFFERENCE of
     * a rectangle and a shape within it, XOR of two shapes (a rectangle, an oval or a rounded rectangle each), INTERSECT
     * of a rectangle and a shape within it, and shapes whose bounds do not meet. Otherwise DIFFERENCE, REVERSE_DIFFERENCE
     * and XOR are the even-odd sum all the same (right wherever the one lies within the other) and INTERSECT is left
     * undone, answered false, as before -- each said once in the log.
     */
    public boolean op(Path p, Op op) {
        if (op == Op.UNION) { addPath(p); return true; }
        RectF a = shapeBounds(this), b = shapeBounds(p);
        switch (op) {
        case DIFFERENCE:
            if (a != null && b != null && !RectF.intersects(a, b)) return true;
            if (a != null && isRectShape(p) && b.contains(a)) { reset(); return true; }
            if (!(isRectShape(this) && b != null && a.contains(b))) saidInexact(op);
            evenOddSum(p);
            return true;
        case REVERSE_DIFFERENCE: {
            if (a != null && b != null && !RectF.intersects(a, b)) { set(p); return true; }
            if (b != null && isRectShape(this) && a.contains(b)) { reset(); return true; }
            if (!(isRectShape(p) && a != null && b.contains(a))) saidInexact(op);
            Path out = new Path(p);
            out.evenOddSum(this);
            set(out);
            return true;
        }
        case XOR:
            if (a == null || b == null) saidInexact(op);
            evenOddSum(p);
            return true;
        case INTERSECT:
            if (a != null && b != null && !RectF.intersects(a, b)) { reset(); return true; }
            if (isRectShape(this) && b != null && a.contains(b)) { set(p); return true; }
            if (isRectShape(p) && a != null && b.contains(a)) return true;
            saidInexact(op);
            return false;
        default:
            return false;
        }
    }
    public boolean op(Path a, Path b, Op op) {
        Path second = b == this ? new Path(b) : b;
        set(a);
        return op(second, op);
    }
    /** This and `p` as one path with the even-odd rule: what one covers and the other does not. */
    private void evenOddSum(Path p) {
        Path q = p == this ? new Path(p) : p;
        addPath(q);
        fill = FillType.EVEN_ODD;
    }
    /** Its bounds when it is one shape alone -- a rectangle, an oval or a rounded rectangle, not inverse -- else null. */
    private static RectF shapeBounds(Path s) {
        if (s.n == 0 || s.isInverseFillType()) return null;
        int op = (int) s.ops[0], len = op == RECT || op == OVAL ? 6 : op == RRECT ? 8 : op == RRECT8 ? 14 : -1;
        if (len < 0 || s.n != len) return null;
        return new RectF(Math.min(s.ops[1], s.ops[3]), Math.min(s.ops[2], s.ops[4]), Math.max(s.ops[1], s.ops[3]), Math.max(s.ops[2], s.ops[4]));
    }
    private static boolean isRectShape(Path s) { return s.n == 6 && (int) s.ops[0] == RECT && !s.isInverseFillType(); }
    private static final java.util.Set<Op> sSaidInexact = java.util.EnumSet.noneOf(Op.class);
    private static void saidInexact(Op op) {
        synchronized (sSaidInexact) {
            if (!sSaidInexact.add(op)) return;
        }
        android.util.Log.w("Path", "Path.op " + op + " of paths this does not combine exactly: "
            + (op == Op.INTERSECT ? "left as it was, answered false" : "the even-odd sum of the two, right where the one lies within the other"));
    }
    public float[] approximate(float acceptableError) { return new float[0]; }
    /** Its segments as they are drawn (PathIterator). */
    public PathIterator getPathIterator() { return new PathIterator(this); }
    /** For Canvas and Region: the fill rule, 0 winding or 1 even-odd, and 2 more when it is inverse. */
    public int meglerFill() { return (fill == FillType.EVEN_ODD || fill == FillType.INVERSE_EVEN_ODD ? 1 : 0) | (isInverseFillType() ? 2 : 0); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public long mNativePath;
    public void computeBounds(android.graphics.RectF p0) {}
    public int getGenerationId() { return 0; }
    public boolean interpolate(android.graphics.Path p0, float p1, android.graphics.Path p2) { return false; }
    public boolean isInterpolatable(android.graphics.Path p0) { return false; }
    public long readOnlyNI() { return 0L; }
    // ---- end of generated members
}
