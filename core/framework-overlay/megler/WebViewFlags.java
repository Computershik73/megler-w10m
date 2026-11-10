package megler;

import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The flags of Chromium's WebView that its developer mode sets: on a phone the WebView DevTools app keeps them, and the WebView
 * asks for them as it starts (android_webview's DeveloperModeUtils and FlagOverrideHelper, called from
 * WebViewChromiumFactoryProvider.initialize before its library is loaded): whether the component
 * org.chromium.android_webview.devui.DeveloperModeState of the WebView's package is enabled (PackageManager
 * .getComponentEnabledSetting), and if so the rows of content://&lt;package&gt;.DeveloperModeContentProvider/flag-overrides,
 * each a flag's name and its state, which it puts on its command line: --enable-features and --disable-features for a feature
 * of Chromium's, the switch itself for a switch. Only the flags of the WebView's own list are taken
 * (ProductionSupportedFlagList; a name it does not have ends the WebView's start), so Megler's are named here, one each.
 *
 * HitTestOpaqueness is off. The WebView of the Android root is AOSP's, not Google's, and a WebView that is not Chrome-branded
 * applies Chromium's field trial testing config wherever no seed says otherwise (variations_field_trial_creator_base.cc,
 * ShouldUseFieldTrialTestingConfig); 128's turns HitTestOpaqueness on for android_webview. With it a layer's recording is
 * bounded by what it paints, not by the layer (cc's RecordingSource, can_use_recorded_bounds), and a page drawn in software --
 * every WebView page on a phone of Direct3D feature level 9_3, where EGL shares no surface's texture and there is no draw
 * functor (drivers/mg_java_webview.c) -- draws each such layer squeezed into those bounds: PictureLayerImpl's resourceless
 * draw gives the quad the visible rectangle cut down to them and the texture coordinates of the whole visible rectangle, and
 * SoftwareRenderer.DrawPictureQuad maps the one onto the other. A layer that paints only part of itself -- a hit area bigger
 * than what is drawn in it, as a Material checkbox's (48 pixels round an 18-pixel box), a text field's, a floating label's --
 * was drawn smaller about its own top left: on a Lumia 650 Google's sign-in page had its password field's label at 0.4 of its
 * size, the password's bullets at a third and the "Show password" box at a third. Drawn on the graphics processor (an HP
 * Elite x3's way) the recorded bounds only say which tiles have something in them, and nothing is squeezed.
 *
 * Megler's flags add the WebView's own diagnostics, one flag each:
 *   webview-layer-borders        show-composited-layer-borders: a border round each of Chromium's layers
 *   webview-verbose-logging      webview-verbose-logging: the WebView's command line and its variations' state in the log
 *   webview-hit-test-opaqueness  HitTestOpaqueness left as the field trial testing config has it (on)
 */
public final class WebViewFlags {
    private WebViewFlags() {}

    /** The developer mode's state, a component of the WebView's package. */
    public static final String STATE = "org.chromium.android_webview.devui.DeveloperModeState";
    private static final String AUTHORITY_SUFFIX = ".DeveloperModeContentProvider", PATH = "/flag-overrides";
    private static Map<String, Boolean> sFlags;

    /**
     * The WebView's flags Megler sets, by name, in the order given; empty when there are none. Only for the WebView these names
     * were checked against (128, the Android root's: tools/webview-fetch.sh): one that left a name out of its list would end
     * its start on it.
     */
    public static synchronized Map<String, Boolean> flags() {
        if (sFlags == null) {
            LinkedHashMap<String, Boolean> f = new LinkedHashMap<>();
            android.content.pm.PackageInfo p = android.webkit.WebView.getCurrentWebViewPackage();
            String version = p != null && p.versionName != null ? p.versionName : "";
            if (!version.startsWith("128.")) {
                android.util.Log.w("Megler", "the WebView's developer-mode flags are not set: they were checked against its version 128, and it is " + version);
            } else {
                if (!Native.flag("webview-hit-test-opaqueness")) f.put("HitTestOpaqueness", false);
                if (Native.flag("webview-layer-borders")) f.put("show-composited-layer-borders", true);
                if (Native.flag("webview-verbose-logging")) f.put("webview-verbose-logging", true);
                if (!f.isEmpty()) android.util.Log.i("Megler", "the WebView's developer-mode flags: " + f);
            }
            sFlags = f;
        }
        return sFlags;
    }

    private static boolean webView(String pkg) {
        android.content.pm.PackageInfo p = android.webkit.WebView.getCurrentWebViewPackage();
        return pkg != null && p != null && pkg.equals(p.packageName);
    }

    /**
     * PackageManager.getComponentEnabledSetting of the WebView's developer mode: enabled while Megler sets flags, as the DevTools
     * app enables it once a flag is set; -1 for any other component.
     */
    public static int state(ComponentName c) {
        if (c == null || !STATE.equals(c.getClassName()) || !webView(c.getPackageName())) return -1;
        return flags().isEmpty() ? PackageManager.COMPONENT_ENABLED_STATE_DEFAULT : PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
    }

    /** The developer mode's provider for its authority, the WebView package's name and ".DeveloperModeContentProvider"; null for any other. */
    public static ContentProvider provider(String authority) {
        if (authority == null || !authority.endsWith(AUTHORITY_SUFFIX)) return null;
        return webView(authority.substring(0, authority.length() - AUTHORITY_SUFFIX.length())) ? PROVIDER : null;
    }

    private static final ContentProvider PROVIDER = new ContentProvider() {
        @Override public boolean onCreate() { return true; }
        @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort) {
            MatrixCursor c = new MatrixCursor(new String[] { "flagName", "flagState" });
            if (u != null && PATH.equals(u.getPath()))
                for (Map.Entry<String, Boolean> e : flags().entrySet()) c.addRow(new Object[] { e.getKey(), e.getValue() ? 1 : 0 });
            return c;
        }
        @Override public String getType(Uri u) { return null; }
        @Override public Uri insert(Uri u, ContentValues v) { return null; }
        @Override public int delete(Uri u, String sel, String[] args) { return 0; }
        @Override public int update(Uri u, ContentValues v, String sel, String[] args) { return 0; }
    };
}
