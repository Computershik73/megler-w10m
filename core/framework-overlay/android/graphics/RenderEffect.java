// megler-w10m overlay of Megler/src/java-framework/gen-src/android/graphics/RenderEffect.java (tools/build-framework.sh), in place
// of the generated stub, whose effects were all empty ones.
//
// What it is: an effect that knows the blur it amounts to. Only blur is drawn: a view with such an effect is drawn into a
// layer that the shell puts onto the picture blurred (android.view.View.drawFromParent, overlaid; megler.h, save_layer_blur).
// A blur whose input is a blur, or two chained, is one blur of both, as two Gaussians make one: their standard deviations
// (0.57735 * radius + 0.5, as Skia takes a radius) add as squares. Every other kind -- a bitmap, a colour filter, an offset,
// a shader, a runtime shader, a blend -- is drawn as if it were not there, the blurs of its inputs still drawn.
//
// Why. Instagram's sign-up screen is pastel patches blurred into one another by RenderEffect.createBlurEffect on views
// (the "filter" of its Bloks and React Native views, View.setRenderEffect), and they were drawn sharp: hard-edged bands.
package android.graphics;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class RenderEffect {
    private final float mBlurX, mBlurY; // Android's radii of the blur this effect amounts to; 0 for none
    private final boolean mClamp;       // what lies beyond the edge is the edge carried on, not transparent (DECAL)

    private RenderEffect(float blurX, float blurY, boolean clamp) {
        mBlurX = blurX;
        mBlurY = blurY;
        mClamp = clamp;
    }

    protected RenderEffect() { this(0, 0, false); }

    /** Megler: the radii of the blur the effect amounts to (0 for none), and whether its edge is clamped. */
    public float meglerBlurX() { return mBlurX; }
    public float meglerBlurY() { return mBlurY; }
    public boolean meglerClamp() { return mClamp; }
    public boolean meglerBlurs() { return mBlurX > 0 || mBlurY > 0; }

    private static float sigma(float radius) { return radius > 0 ? 0.57735f * radius + 0.5f : 0; }
    private static float radius(float sigma) { return sigma > 0.5f ? (sigma - 0.5f) / 0.57735f : 0; }
    /** A blur of radius r over one of radius s: one Gaussian whose variance is the sum of theirs. */
    private static float both(float r, float s) {
        if (r <= 0) return s;
        if (s <= 0) return r;
        float a = sigma(r), b = sigma(s);
        return radius((float) Math.sqrt(a * a + b * b));
    }
    private static RenderEffect over(RenderEffect input, float blurX, float blurY, boolean clamp) {
        if (input == null) return new RenderEffect(blurX, blurY, clamp);
        return new RenderEffect(both(blurX, input.mBlurX), both(blurY, input.mBlurY), blurX > 0 || blurY > 0 ? clamp : input.mClamp);
    }
    private static RenderEffect sameAs(RenderEffect input) { return input == null ? new RenderEffect() : new RenderEffect(input.mBlurX, input.mBlurY, input.mClamp); }

    public static android.graphics.RenderEffect createBlurEffect(float radiusX, float radiusY, android.graphics.Shader.TileMode edge) {
        return over(null, Math.max(0, radiusX), Math.max(0, radiusY), edge != null && edge != android.graphics.Shader.TileMode.DECAL);
    }
    public static android.graphics.RenderEffect createBlurEffect(float radiusX, float radiusY, android.graphics.RenderEffect input, android.graphics.Shader.TileMode edge) {
        return over(input, Math.max(0, radiusX), Math.max(0, radiusY), edge != null && edge != android.graphics.Shader.TileMode.DECAL);
    }
    public static android.graphics.RenderEffect createChainEffect(android.graphics.RenderEffect outer, android.graphics.RenderEffect inner) {
        if (outer == null) return sameAs(inner);
        return over(inner, outer.mBlurX, outer.mBlurY, outer.mClamp);
    }
    public static android.graphics.RenderEffect createColorFilterEffect(android.graphics.ColorFilter filter) { return new RenderEffect(); }
    public static android.graphics.RenderEffect createColorFilterEffect(android.graphics.ColorFilter filter, android.graphics.RenderEffect input) { return sameAs(input); }
    public static android.graphics.RenderEffect createOffsetEffect(float dx, float dy) { return new RenderEffect(); }
    public static android.graphics.RenderEffect createOffsetEffect(float dx, float dy, android.graphics.RenderEffect input) { return sameAs(input); }
    public static android.graphics.RenderEffect createBitmapEffect(android.graphics.Bitmap bitmap) { return new RenderEffect(); }
    public static android.graphics.RenderEffect createBitmapEffect(android.graphics.Bitmap bitmap, android.graphics.Rect src, android.graphics.Rect dst) { return new RenderEffect(); }
    public static android.graphics.RenderEffect createBlendModeEffect(android.graphics.RenderEffect dst, android.graphics.RenderEffect src, android.graphics.BlendMode mode) { return sameAs(src); }
    public static android.graphics.RenderEffect createRuntimeShaderEffect(android.graphics.RuntimeShader shader, java.lang.String uniform) { return new RenderEffect(); }
    public static android.graphics.RenderEffect createShaderEffect(android.graphics.Shader shader) { return new RenderEffect(); }
}
