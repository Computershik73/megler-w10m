// Overlay: android.app.compat.CompatChanges, Android's system API for whether a compatibility change is on for an
// app, new to the original's framework (tools/build-framework.sh).
//
// Why. The WebView's code asks it, on Android 13 and later, whether WebSettings' simplified dark mode (change 214741472) is
// on for the app (com.android.webview.chromium's start, before it sets up AwDarkMode), and ended the WebView's start with
// NoClassDefFoundError without it.
//
// How. Android answers from the platform's database of changes: a change is on unless it is disabled, or enabled only from a
// target SDK the app does not reach. This knows the changes code here has asked about, each with the target SDK it is
// enabled since; any other is on, as a change with neither @Disabled nor @EnabledSince is on Android. The app is the one
// running (Manifest.targetSdk); the package and uid forms answer for it.
package android.app.compat;

/** Whether a compatibility change is on for the app (see the top of the file). */
public final class CompatChanges {
    private CompatChanges() {}

    /** The changes asked about here and the target SDK each is enabled since. */
    private static final long[][] ENABLED_SINCE = {
        { 214741472L, 33 },   // WebSettings.ENABLE_SIMPLIFIED_DARK_MODE, @EnabledSince(TIRAMISU)
    };

    public static boolean isChangeEnabled(long changeId) {
        for (long[] c : ENABLED_SINCE) if (c[0] == changeId) return megler.Manifest.targetSdk >= c[1];
        return true;
    }

    public static boolean isChangeEnabled(long changeId, String packageName, android.os.UserHandle user) { return isChangeEnabled(changeId); }

    public static boolean isChangeEnabled(long changeId, int uid) { return isChangeEnabled(changeId); }
}
