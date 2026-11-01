package android.content.pm;

/**
 * Android's (hidden) observer of PackageManager.freeStorageAndNotify and deleteApplicationCacheFiles. Google Play has a Stub
 * of it of its own and asks for freeStorageAndNotify by reflection before a download; with no such interface here the
 * method could not be declared, and it said "NoSuchMethodException: freeStorageAndNotify".
 */
public interface IPackageDataObserver extends android.os.IInterface {
    void onRemoveCompleted(String packageName, boolean succeeded) throws android.os.RemoteException;
}
