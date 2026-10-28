package dalvik.system;

/**
 * Android 14's check of a ZIP entry's name as libcore's ZipFile and ZipInputStream read it (for an app that targets 34, a
 * name with ".." or a leading '/' is refused). Megler's libcore is Android 9's, which checks no name: the callback an app
 * sets is kept and not called, as Android 14 has it for an app that cleared it. OsmAnd clears it as its PlatformUtil is
 * made, and ended at its start on the missing class.
 */
public final class ZipPathValidator {
    private ZipPathValidator() {}

    public interface Callback {
        void onZipEntryAccess(String path) throws java.util.zip.ZipException;
    }

    private static volatile Callback sCallback;

    public static void clearCallback() { sCallback = null; }

    public static void setCallback(Callback callback) {
        if (callback == null) throw new NullPointerException("callback");
        sCallback = callback;
    }
}
