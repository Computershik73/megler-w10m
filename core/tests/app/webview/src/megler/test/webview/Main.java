package megler.test.webview;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/**
 * A WebView, which is Android's System WebView (Chromium) loaded from the Android root's WebView APK, as an app that logs in
 * through one uses it. An HTTP server of the app's own on 127.0.0.1 serves the pages. In order, each a line of the log:
 *  - provider: the WebView is made; its package and version;
 *  - load: a page from the server is loaded (onPageFinished), and its script has run (its title);
 *  - bridge: the page's script has called an object the app gave it with addJavascriptInterface;
 *  - draw: the WebView drawn into a bitmap of the app's has the page's colours where the page has them;
 *  - js: evaluateJavascript gives the script's value back;
 *  - cookies: a cookie the server set is in CookieManager, a cookie the app set there is sent with the server's cookie on
 *    the next request;
 *  - redirect: a navigation of the page's own is redirected by the server to a callback URL of a scheme of the app's, as an
 *    OAuth server sends a login back, and shouldOverrideUrlLoading is asked about it and takes it;
 *  - input: the page's text field is where the log says (input-at), the test taps it, the field has the focus, the test types
 *    with the keyboard, and the field has the text;
 *  - layers: a page of Chromium's layers that paint only part of themselves (LAYERS), whose boxes the log places (layers-at)
 *    and the test finds whole in the picture; the test's key F12 lets the app go on.
 * The test (core/tests/mg_java_tests.c, run_webview) reads the lines, taps, types, and looks at the picture.
 */
public class Main extends Activity {
    private static final String T = "WebViewTest";
    private final Handler main = new Handler(Looper.getMainLooper());
    private WebView web;
    private Server server;
    private String base;
    private int stage;
    private long started;

    /** What the page's script calls (addJavascriptInterface): on a thread of the WebView's own, as on Android. */
    final class Bridge {
        @JavascriptInterface
        public void report(String value) {
            Log.i(T, "bridge ok value=" + value + " main=" + (Looper.myLooper() == Looper.getMainLooper()));
        }
    }

    private String defaultAgent;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        try {
            server = new Server();
        } catch (Exception e) {
            Log.e(T, "server FAILED", e);
            return;
        }
        base = "http://127.0.0.1:" + server.port;
        started = SystemClock.uptimeMillis();
        // The default User-Agent asked before any WebView is made: answered without loading Chromium, and the same string
        // the WebView itself gives once it is loaded (checked at "user-agent" below).
        defaultAgent = android.webkit.WebSettings.getDefaultUserAgent(this);
        Log.i(T, "default user-agent " + defaultAgent + " ms=" + (SystemClock.uptimeMillis() - started));
        try {
            web = new WebView(this);
        } catch (Throwable t) {
            Log.e(T, "provider FAILED " + t, t);
            return;
        }
        PackageInfo pi = WebView.getCurrentWebViewPackage();
        Log.i(T, "provider ok package=" + (pi != null ? pi.packageName + " " + pi.versionName : "none") + " ms=" + (SystemClock.uptimeMillis() - started));
        glue();
        web.getSettings().setJavaScriptEnabled(true);
        web.addJavascriptInterface(new Bridge(), "megler");
        web.setWebViewClient(new Client());
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage m) {
                Log.i(T, "console " + m.message());
                return true;
            }
        });
        setContentView(web);
        Log.i(T, "user-agent " + web.getSettings().getUserAgentString());
        Log.i(T, "user-agent-default " + (web.getSettings().getUserAgentString().equals(defaultAgent) ? "ok" : "FAILED " + defaultAgent));
        stage = 1;
        web.loadUrl(base + "/page");
    }

    /*
     * androidx.webkit's way to the WebView's support-library glue (WebViewGlueCommunicator): the glue's class by name through
     * WebView.getWebViewClassLoader(), the provider factory it makes, and that factory's features asked through the boundary
     * interface the app carries. Where it is not found, androidx.webkit has no feature of the WebView's: Yandex Maps' profile
     * screen ended in WebSettingsCompat.setAlgorithmicDarkeningAllowed with an UnsupportedOperationException.
     */
    private void glue() {
        try {
            Class<?> c = Class.forName("org.chromium.support_lib_glue.SupportLibReflectionUtil", false, WebView.getWebViewClassLoader());
            Object factory = c.getDeclaredMethod("createWebViewProviderFactory").invoke(null);
            java.lang.reflect.Method m = org.chromium.support_lib_boundary.WebViewProviderFactoryBoundaryInterface.class.getMethod("getSupportedFeatures");
            String[] features = (String[]) ((java.lang.reflect.InvocationHandler) factory).invoke(null, m, null);
            boolean dark = java.util.Arrays.asList(features).contains("ALGORITHMIC_DARKENING");
            Log.i(T, "glue " + (features.length > 0 && dark ? "ok" : "FAILED") + " features=" + features.length + " algorithmic-darkening=" + dark);
        } catch (Throwable t) {
            ClassLoader l = WebView.getWebViewClassLoader();
            String provider;
            try {
                provider = Class.forName("com.android.webview.chromium.WebViewChromiumFactoryProvider", false, l) + " of " + Class.forName("com.android.webview.chromium.WebViewChromiumFactoryProvider", false, l).getClassLoader();
            } catch (Throwable p) {
                provider = p.toString();
            }
            Log.e(T, "glue FAILED " + t + " loader=" + l + " provider=" + provider, t);
        }
    }

    private final class Client extends WebViewClient {
        @Override
        public void onPageStarted(WebView view, String url, Bitmap favicon) {
            Log.i(T, "page-started " + url);
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            Log.i(T, "page-finished " + url + " title=" + view.getTitle() + " ms=" + (SystemClock.uptimeMillis() - started));
            if (stage == 1 && url.endsWith("/page")) {
                stage = 2;
                Log.i(T, "load ok title=" + view.getTitle());
                main.postDelayed(Main.this::draw, 1500);
            } else if (stage == 5 && url.endsWith("/check-cookie")) {
                stage = 6;
                checkCookiePage();
            } else if (stage == 7 && url.endsWith("/page")) {
                stage = 8;
                main.postDelayed(Main.this::input, 500);
            } else if (stage == 9 && url.endsWith("/layers")) {
                stage = 10;
                main.postDelayed(Main.this::layersAt, 800);
            }
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            String url = request.getUrl().toString();
            Log.i(T, "should-override " + url + " redirect=" + request.isRedirect() + " main-frame=" + request.isForMainFrame());
            if (url.startsWith("meglertest://")) {
                if (stage == 6) {
                    Log.i(T, "redirect ok url=" + url + " code=" + request.getUrl().getQueryParameter("code") + " redirect=" + request.isRedirect());
                    stage = 7;
                    main.post(() -> web.loadUrl(base + "/page"));
                }
                return true;
            }
            return false;
        }

        @Override
        public void onReceivedError(WebView view, android.webkit.WebResourceRequest request, android.webkit.WebResourceError error) {
            Log.w(T, "error " + request.getUrl() + " " + error.getErrorCode() + " " + error.getDescription());
        }
    }

    /** The WebView drawn into a bitmap of the app's own: the page's yellow bar at the top, its blue below. */
    private void draw() {
        int w = web.getWidth(), h = web.getHeight();
        if (w <= 0 || h <= 0) {
            Log.i(T, "draw FAILED size=" + w + "x" + h);
            js();
            return;
        }
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        long t0 = SystemClock.uptimeMillis();
        web.draw(new Canvas(b));
        long ms = SystemClock.uptimeMillis() - t0;
        int bar = b.getPixel(w / 2, Math.max(1, (int) (20 * getResources().getDisplayMetrics().density))) & 0xFFFFFF;
        int body = b.getPixel(w / 2, h - 10) & 0xFFFFFF;
        boolean ok = near(bar, 0xFFD200) && near(body, 0x1E78C8);
        Log.i(T, "draw " + (ok ? "ok" : "FAILED") + " size=" + w + "x" + h + " bar=" + hex(bar) + " body=" + hex(body) + " ms=" + ms);
        drawNode(w, h);
        canvas();
    }

    /**
     * A canvas of the page's, 300x150 (the size Chromium asks the platform's hardware buffers for), filled by its script and
     * read back with getImageData; and whether the page has WebGL, and if so a pixel it cleared and read back. The 2D canvas
     * is the step; WebGL is said (a phone's ANGLE has it, the tests' stand-in does not).
     */
    private void canvas() {
        web.evaluateJavascript("(function(){ var c = document.createElement('canvas'); c.width = 300; c.height = 150;"
                + " document.body.appendChild(c); var x = c.getContext('2d'); x.fillStyle = 'rgb(200,30,90)'; x.fillRect(0, 0, 300, 150);"
                + " var p = x.getImageData(150, 75, 1, 1).data; var g = document.createElement('canvas').getContext('webgl'), w = 'no';"
                + " if (g) { g.clearColor(0, 1, 0, 1); g.clear(g.COLOR_BUFFER_BIT); var q = new Uint8Array(4);"
                + " g.readPixels(0, 0, 1, 1, g.RGBA, g.UNSIGNED_BYTE, q); w = 'yes:' + q.join('.'); }"
                + " c.remove(); return p[0] + '.' + p[1] + '.' + p[2] + '.' + p[3] + ' webgl=' + w; })()", value -> {
            String v = value == null ? "" : value.replace("\"", "");
            boolean ok = v.startsWith("200.30.90.255 ");
            Log.i(T, "canvas " + (ok ? "ok" : "FAILED") + " rgba=" + v);
            js();
        });
    }

    /**
     * The WebView drawn through a RenderNode, whose canvas says it is hardware-accelerated, as Compose draws an AndroidView:
     * the node drawn into a bitmap of the app's has the same bar and body.
     */
    private void drawNode(int w, int h) {
        android.graphics.RenderNode node = new android.graphics.RenderNode("webview");
        node.setPosition(0, 0, w, h);
        android.graphics.RecordingCanvas rc = node.beginRecording(w, h);
        boolean hardware = rc.isHardwareAccelerated();
        web.draw(rc);
        node.endRecording();
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        new Canvas(b).drawRenderNode(node);
        int bar = b.getPixel(w / 2, Math.max(1, (int) (20 * getResources().getDisplayMetrics().density))) & 0xFFFFFF;
        int body = b.getPixel(w / 2, h - 10) & 0xFFFFFF;
        boolean ok = hardware && near(bar, 0xFFD200) && near(body, 0x1E78C8);
        Log.i(T, "draw-node " + (ok ? "ok" : "FAILED") + " hardware=" + hardware + " bar=" + hex(bar) + " body=" + hex(body));
    }

    private void js() {
        stage = 3;
        web.evaluateJavascript("(function(){ return 6 * 7; })()", value -> {
            Log.i(T, "js " + ("42".equals(value) ? "ok" : "FAILED") + " value=" + value);
            cookies();
        });
    }

    private void cookies() {
        stage = 4;
        CookieManager cm = CookieManager.getInstance();
        String c = cm.getCookie(base);
        Log.i(T, "cookies-manager " + (c != null && c.contains("megler=1") ? "ok" : "FAILED") + " cookie=" + c);
        cm.setCookie(base, "app=2; path=/");
        cm.flush();
        stage = 5;
        web.loadUrl(base + "/check-cookie");
    }

    private void checkCookiePage() {
        web.evaluateJavascript("document.body.textContent", value -> {
            boolean ok = value != null && value.contains("megler=1") && value.contains("app=2");
            Log.i(T, "cookies " + (ok ? "ok" : "FAILED") + " sent=" + value);
            // A navigation of the page's own, which the server redirects to the app's callback, as a login's last step.
            web.evaluateJavascript("location.href = '/login'; 0", null);
        });
    }

    /** Where the text field is on the screen, for the test to tap; then the field's focus and its text are watched. */
    private void input() {
        web.evaluateJavascript("(function(){ var r = document.getElementById('f').getBoundingClientRect();"
                + " return [r.left + r.width / 2, r.top + r.height / 2, window.devicePixelRatio].join(','); })()", value -> {
            String[] f = value.replace("\"", "").split(",");
            int[] at = new int[2];
            web.getLocationOnScreen(at);
            float dpr = Float.parseFloat(f[2]);
            int x = at[0] + Math.round(Float.parseFloat(f[0]) * dpr), y = at[1] + Math.round(Float.parseFloat(f[1]) * dpr);
            Log.i(T, "input-at " + x + "," + y + " dpr=" + dpr);
            watchInput(0, false);
        });
    }

    private void watchInput(final int tries, final boolean focused) {
        web.evaluateJavascript("(function(){ var f = document.getElementById('f');"
                + " return (document.activeElement === f ? 'focus:' : 'blur:') + f.value; })()", value -> {
            String v = value == null ? "" : value.replace("\"", "");
            boolean now = v.startsWith("focus:");
            if (now && !focused) Log.i(T, "input-focused ok");
            String text = v.substring(v.indexOf(':') + 1);
            if (text.equals("hello")) {
                Log.i(T, "input ok value=" + text);
                layers();
                return;
            }
            if (tries > 200) {
                Log.i(T, "input FAILED focused=" + now + " value=" + text);
                Log.i(T, "done");
                return;
            }
            main.postDelayed(() -> watchInput(tries + 1, focused || now), 300);
        });
    }

    /**
     * Layers that paint only part of themselves (LAYERS): their boxes are where the page says, in the window's pixels
     * ("layers-at"), for the test to look at in the picture; then the app waits for the test's key (KEYCODE_F12) to go on.
     */
    private void layers() {
        stage = 9;
        web.loadUrl(base + "/layers");
    }

    private void layersAt() {
        web.evaluateJavascript("(function(){ var d = window.devicePixelRatio, out = [];"
                + " ['box', 'inner', 'full'].forEach(function(id) { var r = document.getElementById(id).getBoundingClientRect();"
                + " out.push(id + '=' + Math.round(r.left * d) + ',' + Math.round(r.top * d) + ',' + Math.round(r.width * d) + ',' + Math.round(r.height * d)); });"
                + " return out.join(' '); })()", value -> {
            int[] at = new int[2];
            web.getLocationOnScreen(at);
            Log.i(T, "layers-at " + at[0] + "," + at[1] + " " + (value == null ? "" : value.replace("\"", "")));
        });
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent e) {
        if (stage == 10 && e.getKeyCode() == android.view.KeyEvent.KEYCODE_F12) {
            if (e.getAction() == android.view.KeyEvent.ACTION_UP) {
                stage = 11;
                dismiss();
            }
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    /** The rows of the bitmap the overlay's WebView last drew a hardware-accelerated canvas through; -1 if none. */
    private static int drawnRows(WebView v) {
        try {
            java.lang.reflect.Field f = WebView.class.getDeclaredField("mMeglerBitmap");
            f.setAccessible(true);
            Bitmap b = (Bitmap) f.get(v);
            return b != null ? b.getHeight() : -1;
        } catch (Exception e) {
            Log.w(T, "dismiss: " + e);
            return -1;
        }
    }

    /**
     * A second WebView over the first, in the same layout, as an app shows a banner, and 20000 rows tall, as a WebView sized
     * to its page is: its page loaded, drawn through a RenderNode its size (as Compose draws it) three times -- through a
     * bitmap of what the window shows of it, not of its 20000 rows -- then taken out of the layout and destroyed, and drawn
     * once more after that, as a draw that was under way when the banner was dismissed. The app goes on.
     */
    private void dismiss() {
        final WebView banner = new WebView(this);
        final android.view.ViewGroup root = (android.view.ViewGroup) web.getParent();
        // As tall as a page sized to its content: far taller than the window, which shows its top.
        root.addView(banner, new android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT, 20000));
        banner.setWebViewClient(new WebViewClient() {
            boolean once;

            @Override
            public void onPageFinished(WebView view, String url) {
                if (once) return;
                once = true;
                main.postDelayed(() -> {
                    int w = banner.getWidth(), h = banner.getHeight();
                    for (int i = 0; i < 3; i++) drawThroughNode(banner, w, h);
                    // What it was drawn through: the part the window shows, not the 20000 rows.
                    int drawn = drawnRows(banner);
                    root.removeView(banner);
                    // Out of the window, and so with nothing to say what is seen: at most a screen's worth.
                    drawThroughNode(banner, w, h);
                    int detached = drawnRows(banner);
                    banner.destroy();
                    drawThroughNode(banner, w, h);
                    boolean ok = h == 20000 && drawn > 0 && drawn <= root.getHeight() && detached > 0
                            && detached <= getResources().getDisplayMetrics().heightPixels;
                    Log.i(T, "dismiss " + (ok ? "ok" : "FAILED") + " size=" + w + "x" + h + " drawn=" + drawn + " detached=" + detached + " window=" + root.getHeight());
                    Log.i(T, "done");
                }, 500);
            }
        });
        banner.loadUrl(base + "/page");
    }

    private static void drawThroughNode(WebView view, int w, int h) {
        android.graphics.RenderNode node = new android.graphics.RenderNode("banner");
        node.setPosition(0, 0, Math.max(1, w), Math.max(1, h));
        android.graphics.RecordingCanvas rc = node.beginRecording(Math.max(1, w), Math.max(1, h));
        view.draw(rc);
        node.endRecording();
    }

    private static boolean near(int a, int b) {
        for (int s = 0; s < 24; s += 8) if (Math.abs(((a >> s) & 255) - ((b >> s) & 255)) > 8) return false;
        return true;
    }

    private static String hex(int rgb) { return String.format("#%06x", rgb & 0xFFFFFF); }

    @Override
    protected void onDestroy() {
        Log.i(T, "onDestroy");
        if (web != null) web.destroy();
        if (server != null) server.close();
        super.onDestroy();
    }

    /** The app's own HTTP server on 127.0.0.1: the pages, the cookie the WebView sends back, and the login's redirect. */
    final class Server implements Runnable {
        final ServerSocket socket;
        final int port;
        private volatile boolean closed;

        Server() throws Exception {
            socket = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
            port = socket.getLocalPort();
            Thread t = new Thread(this, "page-server");
            t.setDaemon(true);
            t.start();
            Log.i(T, "server on " + port);
        }

        void close() {
            closed = true;
            try { socket.close(); } catch (Exception e) { }
        }

        public void run() {
            while (!closed) {
                final Socket s;
                try {
                    s = socket.accept();
                } catch (Exception e) {
                    if (!closed) Log.w(T, "accept: " + e);
                    return;
                }
                Thread t = new Thread(() -> serve(s), "page-request");
                t.setDaemon(true);
                t.start();
            }
        }

        private void serve(Socket s) {
            try {
                BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.ISO_8859_1));
                String line = in.readLine();
                if (line == null) { s.close(); return; }
                String cookie = "";
                String h;
                while ((h = in.readLine()) != null && !h.isEmpty()) {
                    if (h.regionMatches(true, 0, "Cookie:", 0, 7)) cookie = h.substring(7).trim();
                }
                String[] parts = line.split(" ");
                String path = parts.length > 1 ? parts[1] : "/";
                Log.i(T, "request " + parts[0] + " " + path + " cookie=" + cookie);
                OutputStream out = s.getOutputStream();
                if (path.equals("/page")) {
                    send(out, "200 OK", "text/html; charset=utf-8", PAGE, "Set-Cookie: megler=1; Path=/\r\n");
                } else if (path.equals("/check-cookie")) {
                    send(out, "200 OK", "text/html; charset=utf-8", "<!DOCTYPE html><html><body style=\"background:#1e78c8\">cookie=" + cookie + "</body></html>", "");
                } else if (path.equals("/layers")) {
                    send(out, "200 OK", "text/html; charset=utf-8", LAYERS, "");
                } else if (path.equals("/login")) {
                    send(out, "302 Found", "text/html", "<html><body>redirecting</body></html>", "Location: meglertest://callback?code=abc123\r\n");
                } else {
                    send(out, "404 Not Found", "text/plain", "not found", "");
                }
                s.close();
            } catch (Exception e) {
                Log.w(T, "serve: " + e);
            }
        }

        private void send(OutputStream out, String status, String type, String body, String extra) throws java.io.IOException {
            byte[] b = body.getBytes(StandardCharsets.UTF_8);
            String head = "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + b.length + "\r\nCache-Control: no-store\r\n"
                    + extra + "Connection: close\r\n\r\n";
            out.write(head.getBytes(StandardCharsets.ISO_8859_1));
            out.write(b);
            out.flush();
        }
    }

    /**
     * Layers of Chromium's own (will-change) that paint only part of themselves, as a Material checkbox does (a 48-pixel input
     * that paints nothing round an 18-pixel box: #box), or a box in the middle of an empty layer (#inner), and one that paints
     * all of itself (#full). A page drawn in software (the platform's software table) drew each of the first two squeezed into
     * what it paints while Chromium's HitTestOpaqueness was on (megler.WebViewFlags).
     */
    private static final String LAYERS = "<!DOCTYPE html><html><head>"
            + "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>layers</title>"
            + "<style>html, body { margin: 0; background: #fff; }"
            + " #check { position: absolute; left: 40px; top: 40px; width: 48px; height: 48px; will-change: transform, opacity; }"
            + " #check input { position: absolute; left: 0; top: 0; width: 48px; height: 48px; margin: 0; opacity: 0; }"
            + " #box { position: absolute; left: 15px; top: 15px; width: 18px; height: 18px; background: #d01f7a; }"
            + " #plain { position: absolute; left: 40px; top: 120px; width: 160px; height: 100px; will-change: transform; }"
            + " #inner { position: absolute; left: 40px; top: 20px; width: 60px; height: 60px; background: #1f7ad0; }"
            + " #full { position: absolute; left: 240px; top: 120px; width: 100px; height: 100px; will-change: transform; background: #1fd07a; }</style>"
            + "</head><body><div id=\"check\"><input type=\"checkbox\"><div id=\"box\"></div></div>"
            + "<div id=\"plain\"><div id=\"inner\"></div></div><div id=\"full\"></div></body></html>";

    /** The page: a yellow bar along the top, blue below, a text field; its script names the page and tells the app. */
    private static final String PAGE = "<!DOCTYPE html><html><head>"
            + "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>start</title>"
            + "<style>html, body { margin: 0; height: 100%; background: #1e78c8; color: #fff; font: 20px sans-serif; }"
            + " #bar { position: absolute; left: 0; top: 0; width: 100%; height: 40px; background: #ffd200; }"
            + " #f { position: absolute; left: 20px; top: 120px; width: 200px; height: 40px; font-size: 20px; }</style></head>"
            + "<body><div id=\"bar\"></div><input id=\"f\" type=\"text\" value=\"\">"
            + "<script>document.title = 'loaded'; if (window.megler) megler.report(document.title);</script>"
            + "</body></html>";
}
