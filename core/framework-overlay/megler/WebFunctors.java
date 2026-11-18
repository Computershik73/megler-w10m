package megler;

/**
 * Chromium's WebView drawn on the graphics processor: the platform's draw functor (AwDrawFn), whose table, render thread and
 * pictures are core/src/drivers/mg_java_webview.c's. A WebView's canvas in a view list says it is hardware-accelerated while
 * {@link #on} (android.view.View.drawFromParent); Chromium then has the platform draw its functor
 * (android.webkit.WebViewDelegate.drawWebViewFunctor), which android.webkit.WebView does with {@link #draw} and records as a
 * picture the shell keeps (Canvas.meglerDrawTexture). Where drawing fails again and again, {@link #off} has pages drawn in
 * software from then on, as they were before. A draw that takes long is no failure: the UI thread waits for it to its end;
 * and while the render thread is busy with something else, a WebView shows its last picture ({@link #BUSY}).
 */
public final class WebFunctors {
    private WebFunctors() {}

    /** {@link #draw}'s answer where the render thread did not take the request, which was withdrawn, or was not asked. */
    public static final long BUSY = -1;

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
        if (++sFailures >= 3) off();
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
     * (id | width << 32 | height << 48), the part at the surface's top left; 0 when it could not be drawn. A draw the
     * render thread has begun is waited for to its end, however long Chromium takes; {@link #BUSY} when the render thread
     * did not begin it in four seconds (the request is withdrawn, and the functor is not drawn then), and at once from
     * then until the render thread answers again.
     */
    public static native long draw(int functor, int left, int top, int width, int height);
}
