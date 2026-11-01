// Megler: Android 10's rollback service, which Megler has none of to give: a rollback manager with no rollbacks, as on a
// phone where nothing was installed so that it could be rolled back. Play Store asks for it once its scheduler runs its
// jobs (getSystemService("rollback")) and ended at its start without one ("Android is missing rollback service"). Its own
// file: the framework as it came had no android.content.rollback.
package android.content.rollback;

public final class RollbackManager {
    public static final String EXTRA_STATUS = "android.content.rollback.extra.STATUS";
    public static final String EXTRA_STATUS_MESSAGE = "android.content.rollback.extra.STATUS_MESSAGE";
    public static final int STATUS_SUCCESS = 0, STATUS_FAILURE = 1, STATUS_FAILURE_ROLLBACK_UNAVAILABLE = 2, STATUS_FAILURE_INSTALL = 3;
    public static final String PROPERTY_ROLLBACK_LIFETIME_MILLIS = "rollback_lifetime_in_millis";

    public RollbackManager() {}

    /** None: nothing here was installed so that it could be rolled back. */
    public java.util.List<RollbackInfo> getAvailableRollbacks() { return new java.util.ArrayList<>(); }
    public java.util.List<RollbackInfo> getRecentlyCommittedRollbacks() { return new java.util.ArrayList<>(); }
    /** There is no such rollback: STATUS_FAILURE_ROLLBACK_UNAVAILABLE to the sender, as Android answers for one it has not. */
    public void commitRollback(int rollbackId, java.util.List<android.content.pm.VersionedPackage> causePackages, android.content.IntentSender statusReceiver) {
        if (statusReceiver == null) return;
        android.content.Intent result = new android.content.Intent();
        result.putExtra(EXTRA_STATUS, STATUS_FAILURE_ROLLBACK_UNAVAILABLE);
        result.putExtra(EXTRA_STATUS_MESSAGE, "Rollback unavailable");
        try { statusReceiver.sendIntent(megler.ContextImpl.app(), 0, result, null, null); }
        catch (Exception e) { android.util.Log.w("Megler", "rollback: the status could not be sent: " + e); }
    }
    public void reloadPersistedData() {}
    public void expireRollbackForPackage(String packageName) {}
    public void blockRollbackManager(long millis) {}
    public void snapshotAndRestoreUserData(String packageName, int[] userIds, int appId, long ceDataInode, String seInfo, int token) {}
}
