package android.content.pm;

/**
 * Compile-only declaration: Android 9's framework (platform-framework.dex, which an app of the platform's own has after Megler's)
 * has this interface at run time, with all its methods. Megler's framework names it only as a type: AppGlobals and
 * ActivityThread.getPackageManager hand out megler.PlatformInterfaces' implementation of it.
 * (The stubs are compiled alone, so it does not say here that it extends android.os.IInterface, as the real one does.)
 */
public interface IPackageManager {}
