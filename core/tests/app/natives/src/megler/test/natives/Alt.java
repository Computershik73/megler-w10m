package megler.test.natives;

/**
 * The class whose native method is in libnatives_alt.so (jni/natives_alt.c), which nothing loads by its name: the step
 * own-file-same-name loads another build of it, which the app made itself under the APK's name, by its path.
 */
public class Alt {
    /** The text of the library that was loaded: "natives_alt version 1" in the APK's. */
    static native String version();
}
