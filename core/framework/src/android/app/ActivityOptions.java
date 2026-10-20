package android.app;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
/** Options for starting an activity (animations, launch bounds): kept as set; Meglet's activity launch reads none of them. */
public class ActivityOptions extends android.app.ComponentOptions {
    private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
    public static final int ANIM_CLIP_REVEAL = 11;
    public static final int ANIM_CUSTOM = 1;
    public static final int ANIM_CUSTOM_IN_PLACE = 10;
    public static final int ANIM_DEFAULT = 6;
    public static final int ANIM_FROM_STYLE = 14;
    public static final int ANIM_LAUNCH_TASK_BEHIND = 7;
    public static final int ANIM_NONE = 0;
    public static final int ANIM_OPEN_CROSS_PROFILE_APPS = 12;
    public static final int ANIM_REMOTE_ANIMATION = 13;
    public static final int ANIM_SCALE_UP = 2;
    public static final int ANIM_SCENE_TRANSITION = 5;
    public static final int ANIM_THUMBNAIL_ASPECT_SCALE_DOWN = 9;
    public static final int ANIM_THUMBNAIL_ASPECT_SCALE_UP = 8;
    public static final int ANIM_THUMBNAIL_SCALE_DOWN = 4;
    public static final int ANIM_THUMBNAIL_SCALE_UP = 3;
    public static final int ANIM_UNDEFINED = -1;
    public static final java.lang.String EXTRA_USAGE_TIME_REPORT = "android.activity.usage_time";
    public static final java.lang.String EXTRA_USAGE_TIME_REPORT_PACKAGES = "android.usage_time_packages";
    public static final java.lang.String KEY_ANIM_BACKGROUND_COLOR = "android:activity.backgroundColor";
    public static final java.lang.String KEY_ANIM_ENTER_RES_ID = "android:activity.animEnterRes";
    public static final java.lang.String KEY_ANIM_EXIT_RES_ID = "android:activity.animExitRes";
    public static final java.lang.String KEY_ANIM_HEIGHT = "android:activity.animHeight";
    public static final java.lang.String KEY_ANIM_IN_PLACE_RES_ID = "android:activity.animInPlaceRes";
    public static final java.lang.String KEY_ANIM_START_LISTENER = "android:activity.animStartListener";
    public static final java.lang.String KEY_ANIM_START_X = "android:activity.animStartX";
    public static final java.lang.String KEY_ANIM_START_Y = "android:activity.animStartY";
    public static final java.lang.String KEY_ANIM_THUMBNAIL = "android:activity.animThumbnail";
    public static final java.lang.String KEY_ANIM_TYPE = "android:activity.animType";
    public static final java.lang.String KEY_ANIM_WIDTH = "android:activity.animWidth";
    public static final java.lang.String KEY_LAUNCH_BOUNDS = "android:activity.launchBounds";
    public static final java.lang.String KEY_LAUNCH_COOKIE = "android.activity.launchCookie";
    public static final java.lang.String KEY_LAUNCH_ROOT_TASK_TOKEN = "android.activity.launchRootTaskToken";
    public static final java.lang.String KEY_LAUNCH_TASK_FRAGMENT_TOKEN = "android.activity.launchTaskFragmentToken";
    public static final java.lang.String KEY_LEGACY_PERMISSION_PROMPT_ELIGIBLE = "android:activity.legacyPermissionPromptEligible";
    public static final java.lang.String KEY_PACKAGE_NAME = "android:activity.packageName";
    public static final java.lang.String KEY_SPLASH_SCREEN_THEME = "android.activity.splashScreenTheme";
    public static final java.lang.String KEY_TRANSIENT_LAUNCH = "android.activity.transientLaunch";
    public static final java.lang.String KEY_WINDOWING_LAYER = "android.activity.windowingLayer";
    public static final int MODE_BACKGROUND_ACTIVITY_START_ALLOWED = 1;
    public static final int MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS = 3;
    public static final int MODE_BACKGROUND_ACTIVITY_START_ALLOW_IF_VISIBLE = 4;
    public static final int MODE_BACKGROUND_ACTIVITY_START_COMPAT = -1;
    public static final int MODE_BACKGROUND_ACTIVITY_START_DENIED = 2;
    public static final int MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED = 0;
    public static final int WINDOWING_LAYER_NORMAL_APP = 1;
    public static final int WINDOWING_LAYER_PINNED = 2;
    public static int WINDOWING_LAYER_UNDEFINED;
    public ActivityOptions(android.os.Bundle p0) { super(); }
    public static void abort(android.app.ActivityOptions p0) {}
    public static android.app.ActivityOptions fromBundle(android.os.Bundle p0) { return new ActivityOptions(); }
    public static boolean hasLaunchTargetContainer(android.app.ActivityOptions p0) { return false; }
    public static android.app.ActivityOptions makeBasic() { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeClipRevealAnimation(android.view.View p0, int p1, int p2, int p3, int p4) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomAnimation(android.content.Context p0, int p1, int p2) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomAnimation(android.content.Context p0, int p1, int p2, int p3) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomAnimation(android.content.Context p0, int p1, int p2, int p3, android.os.Handler p4, android.app.ActivityOptions.OnAnimationStartedListener p5) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomAnimation(android.content.Context p0, int p1, int p2, int p3, android.os.Handler p4, android.app.ActivityOptions.OnAnimationStartedListener p5, android.app.ActivityOptions.OnAnimationFinishedListener p6) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomInPlaceAnimation(android.content.Context p0, int p1) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeCustomTaskAnimation(android.content.Context p0, int p1, int p2, android.os.Handler p3, android.app.ActivityOptions.OnAnimationStartedListener p4, android.app.ActivityOptions.OnAnimationFinishedListener p5) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeLaunchIntoPip(android.app.PictureInPictureParams p0) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeMultiThumbFutureAspectScaleAnimation(android.content.Context p0, android.os.Handler p1, android.view.IAppTransitionAnimationSpecsFuture p2, android.app.ActivityOptions.OnAnimationStartedListener p3, boolean p4) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeOpenCrossProfileAppsAnimation() { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeRemoteAnimation(android.view.RemoteAnimationAdapter p0) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeRemoteAnimation(android.view.RemoteAnimationAdapter p0, android.window.RemoteTransition p1) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeRemoteTransition(android.window.RemoteTransition p0) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeScaleUpAnimation(android.view.View p0, int p1, int p2, int p3, int p4) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeSceneTransitionAnimation(android.app.Activity p0, android.view.View p1, java.lang.String p2) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeSceneTransitionAnimation(android.app.Activity p0, android.util.Pair[] p1) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeTaskLaunchBehind() { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeThumbnailAspectScaleDownAnimation(android.view.View p0, android.graphics.Bitmap p1, int p2, int p3, int p4, int p5, android.os.Handler p6, android.app.ActivityOptions.OnAnimationStartedListener p7) { return new ActivityOptions(); }
    public static android.app.ActivityOptions makeThumbnailScaleUpAnimation(android.view.View p0, android.graphics.Bitmap p1, int p2, int p3) { return new ActivityOptions(); }
    public static void setExitTransitionTimeout(long p0) {}
    public static android.util.Pair startSharedElementAnimation(android.view.Window p0, android.app.ExitTransitionCoordinator.ExitTransitionCallbacks p1, android.app.SharedElementCallback p2, android.util.Pair[] p3) { return null; }
    public static void stopSharedElementAnimation(android.view.Window p0) {}
    public void abort() {}
    public boolean canTaskOverlayResume() { return false; }
    public boolean disallowEnterPictureInPictureWhileLaunching() { return false; }
    public android.app.ActivityOptions forTargetActivity() { return this; }
    public boolean freezeRecentTasksReordering() { return false; }
    public android.os.IRemoteCallback getAnimationFinishedListener() { return (android.os.IRemoteCallback) megletProps.get("AnimationFinishedListener"); }
    public android.os.IRemoteCallback getAnimationStartedListener() { return (android.os.IRemoteCallback) megletProps.get("AnimationStartedListener"); }
    public int getAnimationType() { return (megletProps.get("AnimationType") instanceof Integer ? (Integer) megletProps.get("AnimationType") : 0); }
    public boolean getAvoidMoveToFront() { return (megletProps.get("AvoidMoveToFront") instanceof Boolean ? (Boolean) megletProps.get("AvoidMoveToFront") : false); }
    public int getCallerDisplayId() { return (megletProps.get("CallerDisplayId") instanceof Integer ? (Integer) megletProps.get("CallerDisplayId") : 0); }
    public int getCustomBackgroundColor() { return (megletProps.get("CustomBackgroundColor") instanceof Integer ? (Integer) megletProps.get("CustomBackgroundColor") : 0); }
    public int getCustomEnterResId() { return (megletProps.get("CustomEnterResId") instanceof Integer ? (Integer) megletProps.get("CustomEnterResId") : 0); }
    public int getCustomExitResId() { return (megletProps.get("CustomExitResId") instanceof Integer ? (Integer) megletProps.get("CustomExitResId") : 0); }
    public int getCustomInPlaceResId() { return (megletProps.get("CustomInPlaceResId") instanceof Integer ? (Integer) megletProps.get("CustomInPlaceResId") : 0); }
    public boolean getDisableStartingWindow() { return (megletProps.get("DisableStartingWindow") instanceof Boolean ? (Boolean) megletProps.get("DisableStartingWindow") : false); }
    public boolean getDismissKeyguardIfInsecure() { return (megletProps.get("DismissKeyguardIfInsecure") instanceof Boolean ? (Boolean) megletProps.get("DismissKeyguardIfInsecure") : false); }
    public boolean getFlexibleLaunchSize() { return (megletProps.get("FlexibleLaunchSize") instanceof Boolean ? (Boolean) megletProps.get("FlexibleLaunchSize") : false); }
    public int getHeight() { return (megletProps.get("Height") instanceof Integer ? (Integer) megletProps.get("Height") : 0); }
    public int getLaunchActivityType() { return (megletProps.get("LaunchActivityType") instanceof Integer ? (Integer) megletProps.get("LaunchActivityType") : 0); }
    public android.graphics.Rect getLaunchBounds() { return (android.graphics.Rect) megletProps.get("LaunchBounds"); }
    public android.os.IBinder getLaunchCookie() { return (android.os.IBinder) megletProps.get("LaunchCookie"); }
    public int getLaunchDisplayId() { return (megletProps.get("LaunchDisplayId") instanceof Integer ? (Integer) megletProps.get("LaunchDisplayId") : 0); }
    public android.app.PictureInPictureParams getLaunchIntoPipParams() { return (android.app.PictureInPictureParams) megletProps.get("LaunchIntoPipParams"); }
    public boolean getLaunchNextToBubble() { return (megletProps.get("LaunchNextToBubble") instanceof Boolean ? (Boolean) megletProps.get("LaunchNextToBubble") : false); }
    public android.window.WindowContainerToken getLaunchRootTask() { return (android.window.WindowContainerToken) megletProps.get("LaunchRootTask"); }
    public boolean getLaunchTaskBehind() { return (megletProps.get("LaunchTaskBehind") instanceof Boolean ? (Boolean) megletProps.get("LaunchTaskBehind") : false); }
    public android.window.WindowContainerToken getLaunchTaskDisplayArea() { return (android.window.WindowContainerToken) megletProps.get("LaunchTaskDisplayArea"); }
    public int getLaunchTaskDisplayAreaFeatureId() { return (megletProps.get("LaunchTaskDisplayAreaFeatureId") instanceof Integer ? (Integer) megletProps.get("LaunchTaskDisplayAreaFeatureId") : 0); }
    public android.os.IBinder getLaunchTaskFragmentToken() { return (android.os.IBinder) megletProps.get("LaunchTaskFragmentToken"); }
    public int getLaunchTaskId() { return (megletProps.get("LaunchTaskId") instanceof Integer ? (Integer) megletProps.get("LaunchTaskId") : 0); }
    public int getLaunchWindowingMode() { return (megletProps.get("LaunchWindowingMode") instanceof Integer ? (Integer) megletProps.get("LaunchWindowingMode") : 0); }
    public boolean getLaunchedFromBubble() { return (megletProps.get("LaunchedFromBubble") instanceof Boolean ? (Boolean) megletProps.get("LaunchedFromBubble") : false); }
    public boolean getLockTaskMode() { return (megletProps.get("LockTaskMode") instanceof Boolean ? (Boolean) megletProps.get("LockTaskMode") : false); }
    public boolean getOverrideTaskTransition() { return (megletProps.get("OverrideTaskTransition") instanceof Boolean ? (Boolean) megletProps.get("OverrideTaskTransition") : false); }
    public java.lang.String getPackageName() { return (java.lang.String) megletProps.get("PackageName"); }
    public int getPendingIntentBackgroundActivityStartMode() { return (megletProps.get("PendingIntentBackgroundActivityStartMode") instanceof Integer ? (Integer) megletProps.get("PendingIntentBackgroundActivityStartMode") : 0); }
    public int getPendingIntentCreatorBackgroundActivityStartMode() { return (megletProps.get("PendingIntentCreatorBackgroundActivityStartMode") instanceof Integer ? (Integer) megletProps.get("PendingIntentCreatorBackgroundActivityStartMode") : 0); }
    public int getPendingIntentLaunchFlags() { return (megletProps.get("PendingIntentLaunchFlags") instanceof Integer ? (Integer) megletProps.get("PendingIntentLaunchFlags") : 0); }
    public android.view.RemoteAnimationAdapter getRemoteAnimationAdapter() { return (android.view.RemoteAnimationAdapter) megletProps.get("RemoteAnimationAdapter"); }
    public android.window.RemoteTransition getRemoteTransition() { return (android.window.RemoteTransition) megletProps.get("RemoteTransition"); }
    public boolean getRemoveWithTaskOranizer() { return (megletProps.get("RemoveWithTaskOranizer") instanceof Boolean ? (Boolean) megletProps.get("RemoveWithTaskOranizer") : false); }
    public boolean getReparentLeafTaskToTda() { return (megletProps.get("ReparentLeafTaskToTda") instanceof Boolean ? (Boolean) megletProps.get("ReparentLeafTaskToTda") : false); }
    public int getRotationAnimationHint() { return (megletProps.get("RotationAnimationHint") instanceof Integer ? (Integer) megletProps.get("RotationAnimationHint") : 0); }
    public android.app.ActivityOptions.SceneTransitionInfo getSceneTransitionInfo() { return (android.app.ActivityOptions.SceneTransitionInfo) megletProps.get("SceneTransitionInfo"); }
    public android.app.ActivityOptions.SourceInfo getSourceInfo() { return (android.app.ActivityOptions.SourceInfo) megletProps.get("SourceInfo"); }
    public android.view.IAppTransitionAnimationSpecsFuture getSpecsFuture() { return (android.view.IAppTransitionAnimationSpecsFuture) megletProps.get("SpecsFuture"); }
    public int getSplashScreenStyle() { return (megletProps.get("SplashScreenStyle") instanceof Integer ? (Integer) megletProps.get("SplashScreenStyle") : 0); }
    public java.lang.String getSplashScreenThemeResName() { return (java.lang.String) megletProps.get("SplashScreenThemeResName"); }
    public int getStartX() { return (megletProps.get("StartX") instanceof Integer ? (Integer) megletProps.get("StartX") : 0); }
    public int getStartY() { return (megletProps.get("StartY") instanceof Integer ? (Integer) megletProps.get("StartY") : 0); }
    public boolean getTaskAlwaysOnTop() { return (megletProps.get("TaskAlwaysOnTop") instanceof Boolean ? (Boolean) megletProps.get("TaskAlwaysOnTop") : false); }
    public boolean getTaskOverlay() { return (megletProps.get("TaskOverlay") instanceof Boolean ? (Boolean) megletProps.get("TaskOverlay") : false); }
    public android.hardware.HardwareBuffer getThumbnail() { return (android.hardware.HardwareBuffer) megletProps.get("Thumbnail"); }
    public boolean getTransientLaunch() { return (megletProps.get("TransientLaunch") instanceof Boolean ? (Boolean) megletProps.get("TransientLaunch") : false); }
    public android.app.PendingIntent getUsageTimeReport() { return (android.app.PendingIntent) megletProps.get("UsageTimeReport"); }
    public int getWidth() { return (megletProps.get("Width") instanceof Integer ? (Integer) megletProps.get("Width") : 0); }
    public int getWindowingLayer() { return (megletProps.get("WindowingLayer") instanceof Integer ? (Integer) megletProps.get("WindowingLayer") : 0); }
    public boolean isAllowPassThroughOnTouchOutside() { return (megletProps.get("AllowPassThroughOnTouchOutside") instanceof Boolean ? (Boolean) megletProps.get("AllowPassThroughOnTouchOutside") : false); }
    public boolean isApplyActivityFlagsForBubbles() { return (megletProps.get("ApplyActivityFlagsForBubbles") instanceof Boolean ? (Boolean) megletProps.get("ApplyActivityFlagsForBubbles") : false); }
    public boolean isApplyMultipleTaskFlagForShortcut() { return (megletProps.get("ApplyMultipleTaskFlagForShortcut") instanceof Boolean ? (Boolean) megletProps.get("ApplyMultipleTaskFlagForShortcut") : false); }
    public boolean isApplyNoUserActionFlagForShortcut() { return (megletProps.get("ApplyNoUserActionFlagForShortcut") instanceof Boolean ? (Boolean) megletProps.get("ApplyNoUserActionFlagForShortcut") : false); }
    public boolean isEligibleForLegacyPermissionPrompt() { return (megletProps.get("EligibleForLegacyPermissionPrompt") instanceof Boolean ? (Boolean) megletProps.get("EligibleForLegacyPermissionPrompt") : false); }
    public boolean isLaunchIntoPip() { return (megletProps.get("LaunchIntoPip") instanceof Boolean ? (Boolean) megletProps.get("LaunchIntoPip") : false); }
    public boolean isPendingIntentBackgroundActivityLaunchAllowed() { return (megletProps.get("PendingIntentBackgroundActivityLaunchAllowed") instanceof Boolean ? (Boolean) megletProps.get("PendingIntentBackgroundActivityLaunchAllowed") : false); }
    public boolean isShareIdentityEnabled() { return (megletProps.get("ShareIdentityEnabled") instanceof Boolean ? (Boolean) megletProps.get("ShareIdentityEnabled") : false); }
    public android.os.Bundle popAppVerificationBundle() { return null; }
    public void requestUsageTimeReport(android.app.PendingIntent p0) {}
    public void setAllowPassThroughOnTouchOutside(boolean p0) { megletProps.put("AllowPassThroughOnTouchOutside", Boolean.valueOf(p0)); }
    public android.app.ActivityOptions setAppVerificationBundle(android.os.Bundle p0) { megletProps.put("AppVerificationBundle", p0); return this; }
    public void setApplyActivityFlagsForBubbles(boolean p0) { megletProps.put("ApplyActivityFlagsForBubbles", Boolean.valueOf(p0)); }
    public void setApplyMultipleTaskFlagForShortcut(boolean p0) { megletProps.put("ApplyMultipleTaskFlagForShortcut", Boolean.valueOf(p0)); }
    public void setApplyNoUserActionFlagForShortcut(boolean p0) { megletProps.put("ApplyNoUserActionFlagForShortcut", Boolean.valueOf(p0)); }
    public void setAvoidMoveToFront() {}
    public android.app.ActivityOptions setCallerDisplayId(int p0) { megletProps.put("CallerDisplayId", Integer.valueOf(p0)); return this; }
    public void setDisableStartingWindow(boolean p0) { megletProps.put("DisableStartingWindow", Boolean.valueOf(p0)); }
    public void setDisallowEnterPictureInPictureWhileLaunching(boolean p0) { megletProps.put("DisallowEnterPictureInPictureWhileLaunching", Boolean.valueOf(p0)); }
    public void setDismissKeyguardIfInsecure() {}
    public void setEligibleForLegacyPermissionPrompt(boolean p0) { megletProps.put("EligibleForLegacyPermissionPrompt", Boolean.valueOf(p0)); }
    public android.app.ActivityOptions setFlexibleLaunchSize(boolean p0) { megletProps.put("FlexibleLaunchSize", Boolean.valueOf(p0)); return this; }
    public void setFreezeRecentTasksReordering() {}
    public void setLaunchActivityType(int p0) { megletProps.put("LaunchActivityType", Integer.valueOf(p0)); }
    public android.app.ActivityOptions setLaunchBounds(android.graphics.Rect p0) { megletProps.put("LaunchBounds", p0); return this; }
    public void setLaunchCookie(android.app.ActivityOptions.LaunchCookie p0) { megletProps.put("LaunchCookie", p0); }
    public void setLaunchCookie(android.os.IBinder p0) { megletProps.put("LaunchCookie", p0); }
    public android.app.ActivityOptions setLaunchDisplayId(int p0) { megletProps.put("LaunchDisplayId", Integer.valueOf(p0)); return this; }
    public android.app.ActivityOptions setLaunchNextToBubble(boolean p0) { megletProps.put("LaunchNextToBubble", Boolean.valueOf(p0)); return this; }
    public android.app.ActivityOptions setLaunchRootTask(android.window.WindowContainerToken p0) { megletProps.put("LaunchRootTask", p0); return this; }
    public android.app.ActivityOptions setLaunchTaskDisplayArea(android.window.WindowContainerToken p0) { megletProps.put("LaunchTaskDisplayArea", p0); return this; }
    public void setLaunchTaskDisplayAreaFeatureId(int p0) { megletProps.put("LaunchTaskDisplayAreaFeatureId", Integer.valueOf(p0)); }
    public android.app.ActivityOptions setLaunchTaskFragmentToken(android.os.IBinder p0) { megletProps.put("LaunchTaskFragmentToken", p0); return this; }
    public void setLaunchTaskId(int p0) { megletProps.put("LaunchTaskId", Integer.valueOf(p0)); }
    public void setLaunchWindowingMode(int p0) { megletProps.put("LaunchWindowingMode", Integer.valueOf(p0)); }
    public void setLaunchedFromBubble(boolean p0) { megletProps.put("LaunchedFromBubble", Boolean.valueOf(p0)); }
    public android.app.ActivityOptions setLockTaskEnabled(boolean p0) { megletProps.put("LockTaskEnabled", Boolean.valueOf(p0)); return this; }
    public void setOnAnimationAbortListener(android.os.IRemoteCallback p0) { megletProps.put("OnAnimationAbortListener", p0); }
    public void setOnAnimationFinishedListener(android.os.IRemoteCallback p0) { megletProps.put("OnAnimationFinishedListener", p0); }
    public android.app.ActivityOptions setOverrideTaskTransition(boolean p0) { megletProps.put("OverrideTaskTransition", Boolean.valueOf(p0)); return this; }
    public void setPendingIntentBackgroundActivityLaunchAllowed(boolean p0) { megletProps.put("PendingIntentBackgroundActivityLaunchAllowed", Boolean.valueOf(p0)); }
    public android.app.ActivityOptions setPendingIntentBackgroundActivityStartMode(int p0) { megletProps.put("PendingIntentBackgroundActivityStartMode", Integer.valueOf(p0)); return this; }
    public android.app.ActivityOptions setPendingIntentCreatorBackgroundActivityStartMode(int p0) { megletProps.put("PendingIntentCreatorBackgroundActivityStartMode", Integer.valueOf(p0)); return this; }
    public void setPendingIntentLaunchFlags(int p0) { megletProps.put("PendingIntentLaunchFlags", Integer.valueOf(p0)); }
    public void setRemoteAnimationAdapter(android.view.RemoteAnimationAdapter p0) { megletProps.put("RemoteAnimationAdapter", p0); }
    public android.app.ActivityOptions setRemoteTransition(android.window.RemoteTransition p0) { megletProps.put("RemoteTransition", p0); return this; }
    public void setRemoveWithTaskOrganizer(boolean p0) { megletProps.put("RemoveWithTaskOrganizer", Boolean.valueOf(p0)); }
    public void setReparentLeafTaskToTda(boolean p0) { megletProps.put("ReparentLeafTaskToTda", Boolean.valueOf(p0)); }
    public void setRotationAnimationHint(int p0) { megletProps.put("RotationAnimationHint", Integer.valueOf(p0)); }
    public android.app.ActivityOptions setSceneTransitionInfo(android.app.ActivityOptions.SceneTransitionInfo p0) { megletProps.put("SceneTransitionInfo", p0); return this; }
    public android.app.ActivityOptions setShareIdentityEnabled(boolean p0) { megletProps.put("ShareIdentityEnabled", Boolean.valueOf(p0)); return this; }
    public void setSourceInfo(int p0, long p1) {}
    public android.app.ActivityOptions setSplashScreenStyle(int p0) { megletProps.put("SplashScreenStyle", Integer.valueOf(p0)); return this; }
    public void setSplitScreenCreateMode(int p0) { megletProps.put("SplitScreenCreateMode", Integer.valueOf(p0)); }
    public void setTaskAlwaysOnTop(boolean p0) { megletProps.put("TaskAlwaysOnTop", Boolean.valueOf(p0)); }
    public void setTaskOverlay(boolean p0, boolean p1) {}
    public android.app.ActivityOptions setTransientLaunch() { return this; }
    public android.app.ActivityOptions setWindowingLayer(int p0) { megletProps.put("WindowingLayer", Integer.valueOf(p0)); return this; }
    public android.os.Bundle toBundle() { android.os.Bundle b = new android.os.Bundle(); for (java.util.Map.Entry<String, Object> e : megletProps.entrySet()) b.putString("android:activity." + e.getKey(), String.valueOf(e.getValue())); return b; }
    public void update(android.app.ActivityOptions p0) {}
    ActivityOptions() { this((android.os.Bundle) null); }
    public static final class LaunchCookie implements android.os.Parcelable {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        public static android.os.Parcelable.Creator CREATOR;
        public android.os.IBinder binder;
        public LaunchCookie() {}
        public LaunchCookie(java.lang.String p0) {}
        public static android.app.ActivityOptions.LaunchCookie readFromParcel(android.os.Parcel p0) { return new LaunchCookie(); }
        public static void writeToParcel(android.app.ActivityOptions.LaunchCookie p0, android.os.Parcel p1) {}
        public int describeContents() { return 0; }
        public void writeToParcel(android.os.Parcel p0, int p1) {}
    }
    public interface OnAnimationFinishedListener {
        void onAnimationFinished(long p0);
    }
    public interface OnAnimationStartedListener {
        void onAnimationStarted(long p0);
    }
    public static abstract class SceneTransitionInfo implements android.os.Parcelable {
        protected SceneTransitionInfo() {}
    }
    public static class SourceInfo implements android.os.Parcelable {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        public static android.os.Parcelable.Creator CREATOR;
        public static final int TYPE_COMPLICATION = 8;
        public static final int TYPE_DESKTOP_ANIMATION = 5;
        public static final int TYPE_LAUNCHER = 1;
        public static final int TYPE_LOCKSCREEN = 3;
        public static final int TYPE_NOTIFICATION = 2;
        public static final int TYPE_QSS = 6;
        public static final int TYPE_RECENTS_ANIMATION = 4;
        public static final int TYPE_TILE = 7;
        public long eventTimeMs;
        public int type;
        public int describeContents() { return 0; }
        public void writeToParcel(android.os.Parcel p0, int p1) {}
        protected SourceInfo() {}
    }
}
