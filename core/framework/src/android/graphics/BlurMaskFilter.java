package android.graphics;

/**
 * A blur of what a paint fills, as Skia's blur mask filter: the radius (a standard deviation of 0.57735 * radius + 0.5 in the
 * drawing's coordinates) and the style. Paint.setMaskFilter puts both into its packet (P_BLUR, P_BLURSTYLE), and the core draws
 * the shapes and text of such a paint blurred (core/src/drivers/mg_java_gfx.c, "A paint's blur"). It kept nothing, and what it
 * should have blurred was drawn sharp. The Google app (17.6.63) draws its Gemini glows so -- the Gemini screen's glowing
 * rings and borders (drawables of a SweepGradient blurred), the waves of its voice search, AI Mode and Lens (a shape filled
 * three times, blurred by eight, four and one times a radius) -- and Gemini's first screen on an HP Elite x3 (0.1.0.739)
 * had solid purple ovals and a solid pill where it glows. A radius of 0 or less is no blur, as Skia makes no filter of it.
 */
public class BlurMaskFilter extends MaskFilter {
    public enum Blur { NORMAL, SOLID, OUTER, INNER }
    final float mRadius;
    final Blur mStyle;
    public BlurMaskFilter(float radius, Blur style) { mRadius = radius; mStyle = style == null ? Blur.NORMAL : style; }
}
