// Overlay of core/framework/gen-src/android/graphics/PathIterator.java, the generated stub (tools/build-framework.sh).
// It differs in walking the path: the stub had no segments and Path.getPathIterator gave null. Now the path's segments
// are those megler.Gfx draws (Gfx.pathSegments, core/src/drivers/mg_java_gfx.c): moves, lines, quads, cubics and
// closes, the arcs, ovals and round rectangles in cubics, as Android 14's walks Skia's path with SkPath::RawIter -- a
// line, quad or cubic gives its start point first, the current point before it, and a close gives no points. There are
// no conics: Path.conicTo is a quad here.
//
// Why. Compose's androidx.graphics.path walks a path through this on Android 14 and later: the Play Store's app pages
// stopped with a NullPointerException on PathIterator.hasNext() of the null that getPathIterator gave.
package android.graphics;

/** The segments of a path, one after another. */
public class PathIterator implements java.util.Iterator<PathIterator.Segment> {
    public static final int VERB_MOVE = 0, VERB_LINE = 1, VERB_QUAD = 2, VERB_CONIC = 3, VERB_CUBIC = 4, VERB_CLOSE = 5, VERB_DONE = 6;
    private static final int POINTS = 8;

    private final float[] v;
    private int at;
    private float sx, sy, cx, cy;

    PathIterator(Path path) {
        float[] built = path.n == 0 ? null : megler.Gfx.pathSegments(path.ops, path.n);
        v = built != null ? built : new float[0];
    }

    private static int verbOf(float f) {
        switch ((int) f) {
        case 0: return VERB_MOVE;
        case 1: return VERB_LINE;
        case 2: return VERB_QUAD;
        case 3: return VERB_CUBIC;
        default: return VERB_CLOSE;
        }
    }

    private static int count(int verb) { return verb == VERB_MOVE || verb == VERB_LINE ? 2 : verb == VERB_QUAD ? 4 : verb == VERB_CUBIC ? 6 : 0; }

    public boolean hasNext() { return at < v.length; }

    public int peek() { return at < v.length ? verbOf(v[at]) : VERB_DONE; }

    /** The next segment's verb, and its points into points[offset..offset + 8). */
    public int next(float[] points, int offset) {
        if (points == null || offset < 0 || points.length < offset + POINTS)
            throw new ArrayIndexOutOfBoundsException("points array must be able to hold at least 8 entries");
        if (at >= v.length) return VERB_DONE;
        int verb = verbOf(v[at++]), k = count(verb), o = offset;
        if (verb == VERB_MOVE) {
            sx = cx = points[o] = v[at];
            sy = cy = points[o + 1] = v[at + 1];
        } else if (verb == VERB_CLOSE) {
            cx = sx;
            cy = sy;
        } else {
            points[o++] = cx;
            points[o++] = cy;
            System.arraycopy(v, at, points, o, k);
            cx = v[at + k - 2];
            cy = v[at + k - 1];
        }
        at += k;
        return verb;
    }

    public Segment next() {
        float[] points = new float[POINTS];
        int verb = next(points, 0);
        return new Segment(verb, points, 0f);
    }

    /** A verb and its points; a conic's weight, which this path never has. */
    public static class Segment {
        private final int verb;
        private final float[] points;
        private final float conicWeight;

        Segment(int verb, float[] points, float conicWeight) { this.verb = verb; this.points = points; this.conicWeight = conicWeight; }
        public int getVerb() { return verb; }
        public float[] getPoints() { return points; }
        public float getConicWeight() { return conicWeight; }
    }
}
