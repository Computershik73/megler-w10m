package android.os.ext;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step sdk-extensions calls of Android 14's class (android.jar 29 has none). */
public class SdkExtensions {
    public static final int AD_SERVICES = 1000000;
    public static int getExtensionVersion(int sdk) { return 0; }
    public static java.util.Map<Integer, Integer> getAllExtensionVersions() { return null; }
}
