package megler;

/**
 * The processor's ABIs as this layer runs an app's native code: 32-bit ARM's on the phone (armeabi-v7a, and the older
 * armeabi, which every ARMv7 runs), and on desktop Windows its own alone -- x86_64, arm64-v8a or x86 -- for no other
 * processor's code is translated (docs/DESKTOP.md). Build, SystemProperties, Process and the APKs' library folders name
 * them from here. They are read from os.arch, which libcore makes of the C library's uname(): the core's, which says the
 * processor it is built for (armv7l, aarch64, x86_64, i686).
 */
public final class Abis {
    /** The one an app's libraries are loaded for (Build.CPU_ABI). */
    public static final String PRIMARY;
    /** Build.CPU_ABI2: armeabi on the phone; none on the desktop's processors. */
    public static final String SECOND;
    /** Build.SUPPORTED_ABIS, and of them those of 32 and of 64 bits. */
    public static final String[] ALL, ALL_32, ALL_64;
    /** Whether the process's code is 64-bit (Process.is64Bit). */
    public static final boolean IS_64;

    static {
        String arch = System.getProperty("os.arch", "");
        if (arch.equals("x86_64") || arch.equals("amd64")) PRIMARY = "x86_64";
        else if (arch.equals("aarch64") || arch.equals("arm64")) PRIMARY = "arm64-v8a";
        else if (arch.equals("x86") || arch.matches("i[3-6]86")) PRIMARY = "x86";
        else PRIMARY = "armeabi-v7a";
        boolean arm = PRIMARY.equals("armeabi-v7a");
        SECOND = arm ? "armeabi" : "";
        ALL = arm ? new String[] { "armeabi-v7a", "armeabi" } : new String[] { PRIMARY };
        IS_64 = PRIMARY.equals("x86_64") || PRIMARY.equals("arm64-v8a");
        ALL_64 = IS_64 ? ALL : new String[0];
        ALL_32 = IS_64 ? new String[0] : ALL;
    }

    private Abis() {}
}
