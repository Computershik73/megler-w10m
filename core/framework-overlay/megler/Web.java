// megler-w10m overlay of Megler/src/java-framework/src/megler/Web.java (tools/build-framework.sh).
// Nothing is left of it but deliver(), which megler.AppRunner calls for input of phase 9, and which has nothing to do.
//
// Why. The original's megler.Web is a WKWebView under the app's screen, for the original's own android.webkit.WebView on
// iOS; on Windows its natives answered nothing and a WebView was an empty rectangle. Here android.webkit is AOSP's
// (core/framework-overlay/android/webkit) and a WebView is Chromium's, from the WebView APK in the Android root, running
// in the app's own process as Android's System WebView does (docs/JAVA.md, "The WebView"). Nothing posts phase 9.
package megler;

/** The original's WKWebView host. Not used here: a WebView is Chromium's (android.webkit.WebViewFactory). */
public final class Web {
    private Web() {}

    /** Input phase 9: the host's events for web views. There is no such host here, so there are none. */
    public static void deliver() {}
}
