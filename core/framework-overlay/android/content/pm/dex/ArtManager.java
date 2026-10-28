// Megler: Android 9's ART manager (PackageManager.getArtManager, a system API), which has no runtime profiles to give here:
// profiling is off, and a snapshot asked for fails, as on a phone where it is off. Play Store's ArtProfilesUploadJob asks for
// it once its scheduler runs its jobs, and ended at its start without it (NoSuchMethodError: getArtManager). Its own file:
// the framework as it came had no android.content.pm.dex.
package android.content.pm.dex;

public class ArtManager {
    public static final int PROFILE_APPS = 0, PROFILE_BOOT_IMAGE = 1;
    public static final int SNAPSHOT_FAILED_PACKAGE_NOT_FOUND = 0, SNAPSHOT_FAILED_INTERNAL_ERROR = 1, SNAPSHOT_FAILED_CODE_PATH_NOT_FOUND = 2;

    public ArtManager() {}

    /** Off: the runtime here keeps no profiles. */
    public boolean isRuntimeProfilingEnabled(int profileType) { return false; }

    /** Fails, on the executor, as it does where profiling is off. */
    public void snapshotRuntimeProfile(int profileType, String packageName, String codePath, java.util.concurrent.Executor executor,
                                       SnapshotRuntimeProfileCallback callback) {
        if (callback == null) return;
        Runnable fail = () -> callback.onError(SNAPSHOT_FAILED_INTERNAL_ERROR);
        if (executor != null) executor.execute(fail); else fail.run();
    }

    public static String getCurrentProfilePath(String packageName, int userId, String splitName) { return null; }
    public static String getReferenceProfilePath(String packageName, int userId, String splitName) { return null; }
    public static String getProfileName(String splitName) { return splitName == null ? "primary.prof" : splitName + ".split.prof"; }

    public abstract static class SnapshotRuntimeProfileCallback {
        public SnapshotRuntimeProfileCallback() {}
        public abstract void onSuccess(android.os.ParcelFileDescriptor profileReadFd);
        public abstract void onError(int errCode);
    }
}
