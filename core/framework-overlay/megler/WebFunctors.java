package megler;

/**
 * Chromium's WebView drawn on the graphics processor: the platform's draw functor (AwDrawFn), whose table, render thread and
 * pictures are core/src/drivers/mg_java_webview.c's. A WebView's canvas in a view list says it is hardware-accelerated while
 * {@link #on} -- with the flag webview-gpu, while it is tried -- (android.view.View.drawFromParent); Chromium then has the platform draw its functor
 * (android.webkit.WebViewDelegate.drawWebViewFunctor), which android.webkit.WebView does with {@link #draw} and records as a
 * picture the shell keeps (Canvas.meglerDrawTexture). Where drawing fails again and again, {@link #off} has pages drawn in
 * software from then on, as they were before.
 */
public final class WebFunctors {
    private WebFunctors() {}

    private static int sOn = -1;
    private static int sFailures;

    /** Whether Chromium has the draw functor's table and pages are drawn on the graphics processor. */
    public static boolean on() {
        if (sOn < 0) sOn = nativeOn() ? 1 : 0;
        return sOn > 0;
    }

    /** A functor drawn (true) or not (false); after three failures in a row, pages are drawn in software from then on. */
    public static void drew(boolean ok) {
        if (ok) { sFailures = 0; return; }
        if (++sFailures >= 3 || !nativeOn()) off();
    }

    /** Pages drawn in software from now on. */
    public static void off() {
        if (sOn != 0) android.util.Log.w("WebView", "pages are drawn in software from now on: the draw functor could not be drawn");
        sOn = 0;
    }

    private static native boolean nativeOn();

    /**
     * The functor drawn now on the WebView's render thread, the part of its view at (left, top), width x height in the view's
     * coordinates (with its scroll): the shell's id of its picture and the size of the surface it was drawn on
     * (id | width << 32 | height << 48), the part at the surface's top left; 0 when it was not drawn.
     */
    public static native long draw(int functor, int left, int top, int width, int height);
}
