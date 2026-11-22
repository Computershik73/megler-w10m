package android.os;

/**
 * Compile-only declaration: Android 9's framework (platform-framework*.dex, which an app of the platform's own has after
 * Megler's) has this interface at run time. Megler's framework names it only as a type, in the members of Android 9's hidden API that
 * tools/compat/platform-members.py generates for Android's own Settings.
 */
public interface IVoldTaskListener {}
