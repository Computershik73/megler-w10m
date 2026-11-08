// Megler: Android 13's SafetyCenterManager, which the framework did not have: the class, with Safety Center off. Android 13 and
// later register the manager on every phone, and a phone without a Safety Center says it is off: so here
// Context.getSystemService(SafetyCenterManager.class) gives one (megler/ContextImpl.java.members), and isSafetyCenterEnabled()
// is false. Its other public methods, setSafetySourceData and reportSafetySourceError, are not here: the apps that call them
// ask isSafetyCenterEnabled() first, as Android tells them to.
//
// Why. Play Store (52.9) asks getSystemService(SafetyCenterManager.class) on its sign-in screen and handles null; without
// the class, the reference to it ended the app with NoClassDefFoundError: android/safetycenter/SafetyCenterManager
// (package 390 on the phone, after the Google account was removed). And Google Play services 26.41 does not handle null
// on Android 14 (SDK_INT 34): its password manager module, as it starts (credential.manager ModuleInitializer, for the new
// modules' intent), takes the manager with a null check that throws, and only then asks isSafetyCenterEnabled() -- where
// getSystemService answered null, its thread ended on a NullPointerException, and where Google Play services is the app
// itself, its crash handler then ended the whole app (status 0), a second after its Location accuracy screen was shown.
package android.safetycenter;

public final class SafetyCenterManager {
    public static final String ACTION_SAFETY_CENTER = "android.intent.action.SAFETY_CENTER";
    public static final String ACTION_REFRESH_SAFETY_SOURCES = "android.safetycenter.action.REFRESH_SAFETY_SOURCES";
    public static final String ACTION_SAFETY_CENTER_ENABLED_CHANGED = "android.safetycenter.action.SAFETY_CENTER_ENABLED_CHANGED";

    SafetyCenterManager() {}

    public boolean isSafetyCenterEnabled() { return false; }
}
