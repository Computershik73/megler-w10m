// Megler: Android 10's rollback service, which Megler has none of to give: a rollback manager with no rollbacks, as on a
// phone where nothing was installed so that it could be rolled back. Play Store asks for it once its scheduler runs its
// jobs (getSystemService("rollback")) and ended at its start without one ("Android is missing rollback service"). Its own
// file: the framework as it came had no android.content.rollback.
package android.content.rollback;

public final class PackageRollbackInfo {
    private final android.content.pm.VersionedPackage mFrom, mTo;
    public PackageRollbackInfo(android.content.pm.VersionedPackage from, android.content.pm.VersionedPackage to) { mFrom = from; mTo = to; }
    public String getPackageName() { return mFrom != null ? mFrom.getPackageName() : null; }
    public android.content.pm.VersionedPackage getVersionRolledBackFrom() { return mFrom; }
    public android.content.pm.VersionedPackage getVersionRolledBackTo() { return mTo; }
}
