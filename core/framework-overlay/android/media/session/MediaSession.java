// Overlay of core/framework/gen-src/android/media/session/MediaSession.java (tools/build-framework.sh).
// It differs in what a session does with what it is given. The generated stub kept each call in a map and nobody heard of
// it; here the session made active last tells the phone what it plays -- its PlaybackState's state and actions, its
// metadata's title, artist, album and picture (megler.Native.mediaSessionState and mediaSessionMetadata, megler.h's
// megler_media_session_host) -- and Android's media keys, which the phone's media controls send (megler_key), reach that
// session's callback as on Android: Callback.onMediaButtonEvent, whose own answer is Android 9's (play, pause, the two
// skips, stop, the seeks, each when the state allows it). The members are the stub's, all of them.
//
// Why. Windows keeps an app's sound going in the background only while its media controls are taken up (the shell's
// player/MediaControls.cpp); with this, they also show what plays and drive the app: the lock screen's pause pauses
// Spotify, as it would on Android. androidx's MediaSessionCompat makes one of these on API 21 and up.
package android.media.session;

import android.content.Intent;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaSession {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int FLAG_EXCLUSIVE_GLOBAL_PRIORITY = 65536;
    public static final int FLAG_HANDLES_MEDIA_BUTTONS = 1;
    public static final int FLAG_HANDLES_TRANSPORT_CONTROLS = 2;
    public static final int INVALID_PID = -1;
    public static final int INVALID_UID = -1;

    /** The active sessions, the one made active last at the end: it is the one the phone hears from and sends keys to. */
    private static final java.util.ArrayList<MediaSession> sActive = new java.util.ArrayList<>();
    private Callback mCallback;
    private Handler mHandler;
    private PlaybackState mPlaybackState;
    private MediaMetadata mMetadata;
    private boolean mActive;

    public MediaSession(android.content.Context p0, java.lang.String p1) {}
    public MediaSession(android.content.Context p0, java.lang.String p1, android.os.Bundle p2) {}
    protected void finalize() {}
    public java.lang.String getCallingPackage() { return (java.lang.String) meglerProps.get("CallingPackage"); }
    public android.media.session.MediaController getController() { return (android.media.session.MediaController) meglerProps.get("Controller"); }
    public android.media.session.MediaSessionManager.RemoteUserInfo getCurrentControllerInfo() { return (android.media.session.MediaSessionManager.RemoteUserInfo) meglerProps.get("CurrentControllerInfo"); }
    public android.media.session.MediaSession.Token getSessionToken() { return (android.media.session.MediaSession.Token) meglerProps.get("SessionToken"); }
    public boolean isActive() { return mActive; }
    public void notifyRemoteVolumeChanged(android.media.VolumeProvider p0) {}
    public void release() { setActive(false); mCallback = null; }
    public void sendSessionEvent(java.lang.String p0, android.os.Bundle p1) {}
    public void setActive(boolean active) {
        boolean wasTop, isTop;
        synchronized (sActive) {
            wasTop = !sActive.isEmpty() && sActive.get(sActive.size() - 1) == this;
            sActive.remove(this);
            mActive = active;
            if (active) sActive.add(this);
            isTop = active;
        }
        if (isTop) { tellState(); tellMetadata(); }
        else if (wasTop) {
            MediaSession next;
            synchronized (sActive) { next = sActive.isEmpty() ? null : sActive.get(sActive.size() - 1); }
            if (next != null) { next.tellState(); next.tellMetadata(); }
            else megler.Native.mediaSessionState(PlaybackState.STATE_NONE, 0);
        }
    }
    public void setCallback(android.media.session.MediaSession.Callback p0) { setCallback(p0, null); }
    public void setCallback(android.media.session.MediaSession.Callback p0, android.os.Handler p1) {
        if (mCallback != null) mCallback.mSession = null;
        mCallback = p0;
        if (p0 == null) { mHandler = null; return; }
        p0.mSession = this;
        Looper l = Looper.myLooper();
        mHandler = p1 != null ? p1 : new Handler(l != null ? l : Looper.getMainLooper());
    }
    public void setExtras(android.os.Bundle p0) { meglerProps.put("Extras", p0); }
    public void setFlags(int p0) { meglerProps.put("Flags", Integer.valueOf(p0)); }
    public void setMediaButtonBroadcastReceiver(android.content.ComponentName p0) { meglerProps.put("MediaButtonBroadcastReceiver", p0); }
    public void setMediaButtonReceiver(android.app.PendingIntent p0) { meglerProps.put("MediaButtonReceiver", p0); }
    public void setMetadata(android.media.MediaMetadata p0) { mMetadata = p0; if (isTop()) tellMetadata(); }
    public void setPlaybackState(android.media.session.PlaybackState p0) { mPlaybackState = p0; if (isTop()) tellState(); }
    public void setPlaybackToLocal(android.media.AudioAttributes p0) { meglerProps.put("PlaybackToLocal", p0); }
    public void setPlaybackToRemote(android.media.VolumeProvider p0) { meglerProps.put("PlaybackToRemote", p0); }
    public void setQueue(java.util.List p0) { meglerProps.put("Queue", p0); }
    public void setQueueTitle(java.lang.CharSequence p0) { meglerProps.put("QueueTitle", p0); }
    public void setRatingType(int p0) { meglerProps.put("RatingType", Integer.valueOf(p0)); }
    public void setSessionActivity(android.app.PendingIntent p0) { meglerProps.put("SessionActivity", p0); }
    MediaSession() { this((android.content.Context) null, (java.lang.String) null); }

    // ---- what the phone is told, and the keys it sends
    private boolean isTop() {
        synchronized (sActive) { return mActive && !sActive.isEmpty() && sActive.get(sActive.size() - 1) == this; }
    }
    private void tellState() {
        PlaybackState s = mPlaybackState;
        megler.Native.mediaSessionState(s != null ? s.getState() : PlaybackState.STATE_NONE, s != null ? s.getActions() : 0);
    }
    /*
     * What the phone was told last (one set of media controls for the app): told again only when something of it changed.
     * Spotify sets its metadata many times a second while it plays, its picture a new Bitmap each time, and each time the
     * picture was scaled to a new Bitmap left to the collector (its pixels the core's memory, which the collector does not
     * see) and handed to the shell, which made a PNG of it: Spotify's process grew from 723 to 824 MB in ten seconds of AI
     * DJ on a Lumia 650 and Windows ended it (652).
     */
    private static String sToldTitle, sToldArtist, sToldAlbum;
    private static long sToldArtKey;
    private static int[] sToldPixels;
    private static int sToldWidth, sToldHeight;
    private static boolean sTold;
    private void tellMetadata() {
        MediaMetadata m = mMetadata;
        if (m == null) {
            synchronized (MediaSession.class) { sTold = false; sToldPixels = null; sToldArtKey = 0; }
            megler.Native.mediaSessionMetadata(null, null, null, null, 0, 0);
            return;
        }
        String title = first(m, MediaMetadata.METADATA_KEY_TITLE, MediaMetadata.METADATA_KEY_DISPLAY_TITLE);
        String artist = first(m, MediaMetadata.METADATA_KEY_ARTIST, MediaMetadata.METADATA_KEY_ALBUM_ARTIST, MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE);
        String album = first(m, MediaMetadata.METADATA_KEY_ALBUM);
        Bitmap art = null;
        for (String key : new String[] { MediaMetadata.METADATA_KEY_ART, MediaMetadata.METADATA_KEY_ALBUM_ART, MediaMetadata.METADATA_KEY_DISPLAY_ICON }) {
            try { art = m.getBitmap(key); } catch (RuntimeException e) { art = null; }
            if (art != null) break;
        }
        long key = art != null && !art.isRecycled() && art.getWidth() > 0 && art.getHeight() > 0 ? artKey(art) : 0;
        int[] pixels;
        int w, h;
        synchronized (MediaSession.class) {
            if (sTold && key == sToldArtKey && java.util.Objects.equals(title, sToldTitle) && java.util.Objects.equals(artist, sToldArtist)
                && java.util.Objects.equals(album, sToldAlbum)) return;
            if (!sTold || key != sToldArtKey) {
                sToldPixels = null;
                sToldWidth = sToldHeight = 0;
                if (key != 0) {
                    try {
                        float scale = Math.min(1f, 256f / Math.max(art.getWidth(), art.getHeight()));
                        int sw = Math.max(1, Math.round(art.getWidth() * scale)), sh = Math.max(1, Math.round(art.getHeight() * scale));
                        Bitmap b = scale < 1f ? Bitmap.createScaledBitmap(art, sw, sh, true) : art;
                        int[] p = new int[sw * sh];
                        b.getPixels(p, 0, sw, 0, 0, sw, sh);
                        // (The scaled copy is this method's own: its pixels go now, not when the collector comes to it.)
                        if (b != art) b.recycle();
                        sToldPixels = p;
                        sToldWidth = sw;
                        sToldHeight = sh;
                    } catch (RuntimeException e) {
                        sToldPixels = null;
                        sToldWidth = sToldHeight = 0;
                    }
                }
                sToldArtKey = key;
            }
            sToldTitle = title;
            sToldArtist = artist;
            sToldAlbum = album;
            sTold = true;
            pixels = sToldPixels;
            w = sToldWidth;
            h = sToldHeight;
        }
        megler.Native.mediaSessionMetadata(title, artist, album, pixels, w, h);
    }
    /* A picture's fingerprint, cheap: its size and 64 of its pixels on a grid. The same artwork set again as a new Bitmap, as
       Spotify does, has the same one. Never 0. */
    private static long artKey(Bitmap art) {
        int w = art.getWidth(), h = art.getHeight();
        long k = w * 31L + h;
        try {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) k = k * 1000003L + art.getPixel(x * (w - 1) / 7, y * (h - 1) / 7);
        } catch (RuntimeException e) {
            k = k * 1000003L + System.identityHashCode(art);
        }
        return k == 0 ? 1 : k;
    }
    private static String first(MediaMetadata m, String... keys) {
        for (String k : keys) {
            CharSequence t;
            try { t = m.getText(k); } catch (RuntimeException e) { t = null; }
            if (t != null && t.length() > 0) return t.toString();
        }
        return null;
    }

    /** Media keys are Android's media session's: false for any other key, or when no active session has a callback. */
    public static boolean meglerMediaKey(KeyEvent e) {
        if (!KeyEvent.isMediaSessionKey(e.getKeyCode())) return false;
        final MediaSession s;
        synchronized (sActive) {
            MediaSession found = null;
            for (int i = sActive.size() - 1; i >= 0 && found == null; i--) if (sActive.get(i).mCallback != null) found = sActive.get(i);
            s = found;
        }
        if (s == null) return false;
        final Callback cb = s.mCallback;
        final Handler h = s.mHandler;
        final Intent i = new Intent(Intent.ACTION_MEDIA_BUTTON);
        i.putExtra(Intent.EXTRA_KEY_EVENT, e);
        Runnable r = new Runnable() { public void run() { if (s.mCallback == cb) cb.onMediaButtonEvent(i); } };
        if (h != null) h.post(r); else r.run();
        return true;
    }

    public static abstract class Callback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        MediaSession mSession;
        public Callback() {}
        public void onCommand(java.lang.String p0, android.os.Bundle p1, android.os.ResultReceiver p2) {}
        public void onCustomAction(java.lang.String p0, android.os.Bundle p1) {}
        public void onFastForward() {}
        /** Android 9's: a key down for which the session's PlaybackState allows the action, made that call. */
        public boolean onMediaButtonEvent(android.content.Intent i) {
            MediaSession s = mSession;
            if (s == null || i == null || !Intent.ACTION_MEDIA_BUTTON.equals(i.getAction())) return false;
            KeyEvent ke = i.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
            if (ke == null || ke.getAction() != KeyEvent.ACTION_DOWN) return false;
            PlaybackState state = s.mPlaybackState;
            long valid = state == null ? 0 : state.getActions();
            switch (ke.getKeyCode()) {
                case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                case KeyEvent.KEYCODE_HEADSETHOOK: {
                    boolean playing = state != null && state.getState() == PlaybackState.STATE_PLAYING;
                    boolean canPlay = (valid & (PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PLAY)) != 0;
                    boolean canPause = (valid & (PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PAUSE)) != 0;
                    if (playing && canPause) onPause();
                    else if (!playing && canPlay) onPlay();
                    return true;
                }
                case KeyEvent.KEYCODE_MEDIA_PLAY:
                    if ((valid & PlaybackState.ACTION_PLAY) != 0) { onPlay(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_PAUSE:
                    if ((valid & PlaybackState.ACTION_PAUSE) != 0) { onPause(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_NEXT:
                    if ((valid & PlaybackState.ACTION_SKIP_TO_NEXT) != 0) { onSkipToNext(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                    if ((valid & PlaybackState.ACTION_SKIP_TO_PREVIOUS) != 0) { onSkipToPrevious(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_STOP:
                    if ((valid & PlaybackState.ACTION_STOP) != 0) { onStop(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD:
                    if ((valid & PlaybackState.ACTION_FAST_FORWARD) != 0) { onFastForward(); return true; }
                    break;
                case KeyEvent.KEYCODE_MEDIA_REWIND:
                    if ((valid & PlaybackState.ACTION_REWIND) != 0) { onRewind(); return true; }
                    break;
            }
            return false;
        }
        public void onPause() {}
        public void onPlay() {}
        public void onPlayFromMediaId(java.lang.String p0, android.os.Bundle p1) {}
        public void onPlayFromSearch(java.lang.String p0, android.os.Bundle p1) {}
        public void onPlayFromUri(android.net.Uri p0, android.os.Bundle p1) {}
        public void onPrepare() {}
        public void onPrepareFromMediaId(java.lang.String p0, android.os.Bundle p1) {}
        public void onPrepareFromSearch(java.lang.String p0, android.os.Bundle p1) {}
        public void onPrepareFromUri(android.net.Uri p0, android.os.Bundle p1) {}
        public void onRewind() {}
        public void onSeekTo(long p0) {}
        public void onSetPlaybackSpeed(float p0) {}
        public void onSetRating(android.media.Rating p0) {}
        public void onSkipToNext() {}
        public void onSkipToPrevious() {}
        public void onSkipToQueueItem(long p0) {}
        public void onStop() {}
    }
    public static abstract class CallbackStub extends android.media.session.ISessionCallback.Stub {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public CallbackStub(android.media.session.MediaSession p0) { super(); }
        public void onAdjustVolume(java.lang.String p0, int p1, int p2, int p3) {}
        public void onCommand(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4, android.os.ResultReceiver p5) {}
        public void onCustomAction(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4) {}
        public void onFastForward(java.lang.String p0, int p1, int p2) {}
        public void onMediaButton(java.lang.String p0, int p1, int p2, android.content.Intent p3, int p4, android.os.ResultReceiver p5) {}
        public void onMediaButtonFromController(java.lang.String p0, int p1, int p2, android.content.Intent p3) {}
        public void onNext(java.lang.String p0, int p1, int p2) {}
        public void onPause(java.lang.String p0, int p1, int p2) {}
        public void onPlay(java.lang.String p0, int p1, int p2) {}
        public void onPlayFromMediaId(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4) {}
        public void onPlayFromSearch(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4) {}
        public void onPlayFromUri(java.lang.String p0, int p1, int p2, android.net.Uri p3, android.os.Bundle p4) {}
        public void onPrepare(java.lang.String p0, int p1, int p2) {}
        public void onPrepareFromMediaId(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4) {}
        public void onPrepareFromSearch(java.lang.String p0, int p1, int p2, java.lang.String p3, android.os.Bundle p4) {}
        public void onPrepareFromUri(java.lang.String p0, int p1, int p2, android.net.Uri p3, android.os.Bundle p4) {}
        public void onPrevious(java.lang.String p0, int p1, int p2) {}
        public void onRate(java.lang.String p0, int p1, int p2, android.media.Rating p3) {}
        public void onRewind(java.lang.String p0, int p1, int p2) {}
        public void onSeekTo(java.lang.String p0, int p1, int p2, long p3) {}
        public void onSetPlaybackSpeed(java.lang.String p0, int p1, int p2, float p3) {}
        public void onSetVolumeTo(java.lang.String p0, int p1, int p2, int p3) {}
        public void onSkipToTrack(java.lang.String p0, int p1, int p2, long p3) {}
        public void onStop(java.lang.String p0, int p1, int p2) {}
        CallbackStub() { this((android.media.session.MediaSession) null); }
    }
    public static final class QueueItem implements android.os.Parcelable {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public static android.os.Parcelable.Creator CREATOR;
        public static final int UNKNOWN_ID = -1;
        public QueueItem(android.media.MediaDescription p0, long p1) {}
        public int describeContents() { return 0; }
        public android.media.MediaDescription getDescription() { return (android.media.MediaDescription) meglerProps.get("Description"); }
        public long getQueueId() { return (meglerProps.get("QueueId") instanceof Long ? (Long) meglerProps.get("QueueId") : 0L); }
        public void writeToParcel(android.os.Parcel p0, int p1) {}
        QueueItem() { this((android.media.MediaDescription) null, (long) 0L); }
    }
    public static final class Token implements android.os.Parcelable {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public static android.os.Parcelable.Creator CREATOR;
        public Token(int p0, android.media.session.ISessionController p1) {}
        public int describeContents() { return 0; }
        public android.media.session.ISessionController getBinder() { return (android.media.session.ISessionController) meglerProps.get("Binder"); }
        public int getUid() { return (meglerProps.get("Uid") instanceof Integer ? (Integer) meglerProps.get("Uid") : 0); }
        public void writeToParcel(android.os.Parcel p0, int p1) {}
        Token() { this((int) 0, (android.media.session.ISessionController) null); }
    }
}
