package android.os.ext;

import android.os.Build;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * Megler: the SDK extensions of the Android Megler says it is, Android 14 as it came: 7 for R, S, T and U alike. The
 * generated stub said 0 of each, which no Android 14 ever says, and code that asks the extension for an API of Android 14's
 * own took it for an older Android without it. The Google Play Store's build for Android 14 (54.3.08-34, the one Play gives a
 * phone of SDK 34) carries no Cronet of its own, and takes Android's HttpEngine only where the S extension is 7 or more --
 * the level that brought android.net.http to Android 12 and 13: here it went to the Cronet it no longer carries,
 * "Cronet native library failed to load; Possible ABI mismatch.", and fell back to OkHttp. HttpEngine is Megler's own engine
 * (megler.http, docs/JAVA.md "Android's HTTP engine"); Cronet's own provider list, which an app carrying Cronet's API makes,
 * offers it now too, and ranks it below an app's own Cronet and Google Play services' (its version is 1.0.0.0, below the
 * 133 that list prefers). The AdServices extension is 0: Megler has no AdServices, and an app asks that one before it
 * uses them. As Android's, a version before R has no extensions to ask about.
 */
public class SdkExtensions {
    public static final int AD_SERVICES = 1000000;

    /** Android 14's level at its release, of R, S, T and U alike. */
    private static final int ANDROID_14 = 7;

    private static final Map<Integer, Integer> ALL;

    static {
        HashMap<Integer, Integer> all = new HashMap<>();
        all.put(Build.VERSION_CODES.R, ANDROID_14);
        all.put(Build.VERSION_CODES.S, ANDROID_14);
        all.put(Build.VERSION_CODES.TIRAMISU, ANDROID_14);
        all.put(Build.VERSION_CODES.UPSIDE_DOWN_CAKE, ANDROID_14);
        all.put(AD_SERVICES, 0);
        ALL = Collections.unmodifiableMap(all);
    }

    protected SdkExtensions() {}

    public static int getExtensionVersion(int sdk) {
        if (sdk < Build.VERSION_CODES.R) throw new IllegalArgumentException(String.valueOf(sdk) + " does not have extensions");
        Integer level = ALL.get(sdk);
        return level == null ? 0 : level;
    }

    public static Map<Integer, Integer> getAllExtensionVersions() {
        return ALL;
    }
}
