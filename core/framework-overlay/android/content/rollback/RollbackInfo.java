// Megler: Android 10's rollback service, which Megler has none of to give: a rollback manager with no rollbacks, as on a
// phone where nothing was installed so that it could be rolled back. Play Store asks for it once its scheduler runs its
// jobs (getSystemService("rollback")) and ended at its start without one ("Android is missing rollback service"). Its own
// file: the framework as it came had no android.content.rollback.
package android.content.rollback;

public final class RollbackInfo {
    private final int mId;
    private final java.util.List<PackageRollbackInfo> mPackages;
    private final boolean mStaged;
    private final java.util.List<android.content.pm.VersionedPackage> mCause;
    private final int mCommittedSessionId;
    public RollbackInfo(int id, java.util.List<PackageRollbackInfo> packages, boolean staged, java.util.List<android.content.pm.VersionedPackage> cause, int committedSessionId) {
        mId = id; mPackages = packages; mStaged = staged; mCause = cause; mCommittedSessionId = committedSessionId;
    }
    public int getRollbackId() { return mId; }
    public java.util.List<PackageRollbackInfo> getPackages() { return mPackages; }
    public boolean isStaged() { return mStaged; }
    public java.util.List<android.content.pm.VersionedPackage> getCausePackages() { return mCause; }
    public int getCommittedSessionId() { return mCommittedSessionId; }
}
