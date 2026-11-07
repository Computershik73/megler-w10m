// Overlay of core/framework/gen-src/android/media/session/MediaSessionManager.java (tools/build-framework.sh).
// It differs in RemoteUserInfo, which is Android's: a final class an app makes, of a package name, a pid and a uid, with
// their getters, equals and hashCode. The generated stub left it an empty abstract class, and AndroidX Media's
// MediaSessionManager.RemoteUserInfo makes one on API 28 and later (from MediaControllerCompat.registerCallback): Deezer
// (package 581) ended at its start with an InstantiationError. Everything else is the generated stub's, unchanged.
package android.media.session;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaSessionManager {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int RESULT_MEDIA_KEY_HANDLED = 1;
    public static final int RESULT_MEDIA_KEY_NOT_HANDLED = 0;
    public MediaSessionManager(android.content.Context p0) {}
    public void addOnActiveSessionsChangedListener(android.content.ComponentName p0, android.os.UserHandle p1, java.util.concurrent.Executor p2, android.media.session.MediaSessionManager.OnActiveSessionsChangedListener p3) {}
    public void addOnActiveSessionsChangedListener(android.media.session.MediaSessionManager.OnActiveSessionsChangedListener p0, android.content.ComponentName p1) {}
    public void addOnActiveSessionsChangedListener(android.media.session.MediaSessionManager.OnActiveSessionsChangedListener p0, android.content.ComponentName p1, android.os.Handler p2) {}
    public void addOnMediaKeyEventDispatchedListener(java.util.concurrent.Executor p0, android.media.session.MediaSessionManager.OnMediaKeyEventDispatchedListener p1) {}
    public void addOnMediaKeyEventSessionChangedListener(java.util.concurrent.Executor p0, android.media.session.MediaSessionManager.OnMediaKeyEventSessionChangedListener p1) {}
    public void addOnSession2TokensChangedListener(android.media.session.MediaSessionManager.OnSession2TokensChangedListener p0) {}
    public void addOnSession2TokensChangedListener(android.media.session.MediaSessionManager.OnSession2TokensChangedListener p0, android.os.Handler p1) {}
    public void addOnSession2TokensChangedListener(android.os.UserHandle p0, android.media.session.MediaSessionManager.OnSession2TokensChangedListener p1, java.util.concurrent.Executor p2) {}
    public android.media.session.ISession createSession(android.media.session.MediaSession.CallbackStub p0, java.lang.String p1, android.os.Bundle p2) { return null; }
    public void dispatchAdjustVolume(int p0, int p1, int p2) {}
    public void dispatchMediaKeyEvent(android.view.KeyEvent p0, boolean p1) {}
    public void dispatchMediaKeyEventAsSystemService(android.view.KeyEvent p0) {}
    public boolean dispatchMediaKeyEventToSessionAsSystemService(android.view.KeyEvent p0, android.media.session.MediaSession.Token p1) { return false; }
    public void dispatchVolumeKeyEvent(android.view.KeyEvent p0, int p1, boolean p2) {}
    public void dispatchVolumeKeyEventAsSystemService(android.view.KeyEvent p0, int p1) {}
    public void dispatchVolumeKeyEventToSessionAsSystemService(android.view.KeyEvent p0, android.media.session.MediaSession.Token p1) {}
    public java.util.List getActiveSessions(android.content.ComponentName p0) { return new java.util.ArrayList(); }
    public java.util.List getActiveSessionsForUser(android.content.ComponentName p0, android.os.UserHandle p1) { return new java.util.ArrayList(); }
    public android.media.session.MediaSession.Token getMediaKeyEventSession() { return (android.media.session.MediaSession.Token) meglerProps.get("MediaKeyEventSession"); }
    public java.lang.String getMediaKeyEventSessionPackageName() { return (java.lang.String) meglerProps.get("MediaKeyEventSessionPackageName"); }
    public java.util.List getSession2Tokens() { return (meglerProps.get("Session2Tokens") != null ? (java.util.List) meglerProps.get("Session2Tokens") : new java.util.ArrayList()); }
    public int getSessionPolicies(android.media.session.MediaSession.Token p0) { return 0; }
    public boolean hasCustomMediaKeyDispatcher(java.lang.String p0) { return false; }
    public boolean hasCustomMediaSessionPolicyProvider(java.lang.String p0) { return false; }
    public boolean isGlobalPriorityActive() { return (meglerProps.get("GlobalPriorityActive") instanceof Boolean ? (Boolean) meglerProps.get("GlobalPriorityActive") : false); }
    public boolean isTrustedForMediaControl(android.media.session.MediaSessionManager.RemoteUserInfo p0) { return false; }
    public void notifySession2Created(android.media.Session2Token p0) {}
    public void registerRemoteSessionCallback(java.util.concurrent.Executor p0, android.media.session.MediaSessionManager.RemoteSessionCallback p1) {}
    public void removeOnActiveSessionsChangedListener(android.media.session.MediaSessionManager.OnActiveSessionsChangedListener p0) {}
    public void removeOnMediaKeyEventDispatchedListener(android.media.session.MediaSessionManager.OnMediaKeyEventDispatchedListener p0) {}
    public void removeOnMediaKeyEventSessionChangedListener(android.media.session.MediaSessionManager.OnMediaKeyEventSessionChangedListener p0) {}
    public void removeOnSession2TokensChangedListener(android.media.session.MediaSessionManager.OnSession2TokensChangedListener p0) {}
    public void setCustomMediaKeyDispatcher(java.lang.String p0) { meglerProps.put("CustomMediaKeyDispatcher", p0); }
    public void setCustomMediaSessionPolicyProvider(java.lang.String p0) { meglerProps.put("CustomMediaSessionPolicyProvider", p0); }
    public void setOnMediaKeyListener(android.media.session.MediaSessionManager.OnMediaKeyListener p0, android.os.Handler p1) {}
    public void setOnVolumeKeyLongPressListener(android.media.session.MediaSessionManager.OnVolumeKeyLongPressListener p0, android.os.Handler p1) {}
    public void setSessionPolicies(android.media.session.MediaSession.Token p0, int p1) {}
    public void unregisterRemoteSessionCallback(android.media.session.MediaSessionManager.RemoteSessionCallback p0) {}
    MediaSessionManager() { this((android.content.Context) null); }
    public interface OnActiveSessionsChangedListener {
        void onActiveSessionsChanged(java.util.List p0);
    }
    public interface OnMediaKeyEventDispatchedListener {
        void onMediaKeyEventDispatched(android.view.KeyEvent p0, java.lang.String p1, android.media.session.MediaSession.Token p2);
    }
    public interface OnMediaKeyEventSessionChangedListener {
        void onMediaKeyEventSessionChanged(java.lang.String p0, android.media.session.MediaSession.Token p1);
    }
    public interface OnMediaKeyListener {
        boolean onMediaKey(android.view.KeyEvent p0);
    }
    public interface OnSession2TokensChangedListener {
        void onSession2TokensChanged(java.util.List p0);
    }
    public interface OnVolumeKeyLongPressListener {
        void onVolumeKeyLongPress(android.view.KeyEvent p0);
    }
    public interface RemoteSessionCallback {
        void onDefaultRemoteSessionChanged(android.media.session.MediaSession.Token p0);
        void onVolumeChanged(android.media.session.MediaSession.Token p0, int p1);
    }
    public static final class RemoteUserInfo {
        private final java.lang.String mPackageName;
        private final int mPid, mUid;
        public RemoteUserInfo(java.lang.String packageName, int pid, int uid) { mPackageName = packageName; mPid = pid; mUid = uid; }
        public java.lang.String getPackageName() { return mPackageName; }
        public int getPid() { return mPid; }
        public int getUid() { return mUid; }
        @Override public boolean equals(java.lang.Object o) {
            if (!(o instanceof RemoteUserInfo)) return false;
            RemoteUserInfo r = (RemoteUserInfo) o;
            return android.text.TextUtils.equals(mPackageName, r.mPackageName) && mPid == r.mPid && mUid == r.mUid;
        }
        @Override public int hashCode() { return java.util.Objects.hash(mPackageName, mPid, mUid); }
    }
}
