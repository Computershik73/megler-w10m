// Megler: Android 13's SafetyCenterManager, which the framework did not have: the class, with Safety Center off. The phone
// has no Safety Center, so Context.getSystemService(SafetyCenterManager.class) answers null, as on a device without one,
// and an app that asks the class itself hears that it is not enabled.
//
// Why. Play Store (52.9) asks getSystemService(SafetyCenterManager.class) on its sign-in screen and handles null; without
// the class, the reference to it ended the app with NoClassDefFoundError: android/safetycenter/SafetyCenterManager
// (package 390 on the phone, after the Google account was removed).
package android.safetycenter;

public final class SafetyCenterManager {
    public static final String ACTION_SAFETY_CENTER = "android.intent.action.SAFETY_CENTER";
    public static final String ACTION_REFRESH_SAFETY_SOURCES = "android.safetycenter.action.REFRESH_SAFETY_SOURCES";
    public static final String ACTION_SAFETY_CENTER_ENABLED_CHANGED = "android.safetycenter.action.SAFETY_CENTER_ENABLED_CHANGED";

    SafetyCenterManager() {}

    public boolean isSafetyCenterEnabled() { return false; }
}
