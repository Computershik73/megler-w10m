// Overlay of core/framework/gen-src/android/media/session/MediaSession.java (tools/build-framework.sh).
// It differs in what a session does with what it is given. The generated stub kept each call in a map and nobody heard of
// it; here the session made active last tells the phone what it plays -- its PlaybackState's state and actions, its
// metadata's title, artist, album and picture (megler.Native.mediaSessionState and mediaSessionMetadata, megler.h's
// megler_media_session_host) -- and Android's media keys, which the phone's media controls send (megler_key), reach that
// session's callback as on Android: Callback.onMediaButtonEvent, whose own answer is Android 9's (play, pause, the two
// skips, stop, the seeks, each when the state allows it). The members are the stub's, all of them.
//
// And a session has its token and its controllers, as Android's has: getSessionToken() is a token of this session (through
// a Parcel too), a MediaController made of it (MediaController.java, or getController()) reads what the session was given
// -- state, metadata, queue, extras, flags, the session's activity -- hears of each change, and what it asks (its
// TransportControls, sendCommand, a media button, the volume of a remote session) reaches the session's Callback: later, on
// the thread of the handler the callback was set with, with the controller's package to be asked for
// (getCurrentControllerInfo). The token and the controller were null, and a queue item kept nothing.
//
// Why. Windows keeps an app's sound going in the background only while its media controls are taken up (the shell's
// player/MediaControls.cpp); with this, they also show what plays and drive the app: the lock screen's pause pauses
// Spotify, as it would on Android. androidx's MediaSessionCompat makes one of these on API 21 and up. And an app may play
// through its own session: Deezer's player screen holds a MediaControllerCompat of the session its playback service made,
// and its play button is TransportControls.play() -- with no token there was no controller to carry it (a
// NullPointerException inside the app, which it swallowed), the service's Callback.onPlay never ran, and nothing sounded
// (0.1.0.690).
package android.media.session;

import android.content.Intent;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaSession {
    public static final int FLAG_EXCLUSIVE_GLOBAL_PRIORITY = 65536;
    public static final int FLAG_HANDLES_MEDIA_BUTTONS = 1;
    public static final int FLAG_HANDLES_TRANSPORT_CONTROLS = 2;
    public static final int INVALID_PID = -1;
    public static final int INVALID_UID = -1;

    /** The active sessions, the one made active last at the end: it is the one the phone hears from and sends keys to. */
    private static final java.util.ArrayList<MediaSession> sActive = new java.util.ArrayList<>();
    private final String mTag, mPackageName;
    private final android.os.Bundle mSessionInfo;
    private final Token mToken;
    private final MediaController mController;
    private volatile Callback mCallback;
    /** The callback's own handler, on the looper of the one it was set with: what an app does to its handler leaves it be. */
    private volatile Handler mHandler;
    private volatile PlaybackState mPlaybackState;
    private volatile MediaMetadata mMetadata;
    private volatile java.util.List<QueueItem> mQueue;
    private volatile CharSequence mQueueTitle;
    private volatile android.os.Bundle mExtras;
    private volatile int mFlags, mRatingType;
    private volatile android.app.PendingIntent mSessionActivity;
    private volatile android.media.AudioAttributes mAudioAttributes;
    private volatile android.media.VolumeProvider mVolumeProvider;
    private volatile boolean mReleased;
    private boolean mActive;
    /** The controllers with a callback registered: each is told what changes. Its lock is the callback's, too. */
    private final java.util.concurrent.CopyOnWriteArrayList<MediaController> mControllers = new java.util.concurrent.CopyOnWriteArrayList<>();

    /** What a token holds: the session's side of its controllers, a binder of this process that knows its session. */
    private static final class ControllerBinder extends android.os.Binder implements ISessionController {
        final MediaSession mSession;
        ControllerBinder(MediaSession session) { mSession = session; }
        public android.os.IBinder asBinder() { return this; }
    }
    /** The session a token is of; null for a token that is not one of this process's sessions. */
    static MediaSession of(Token token) {
        ISessionController binder = token == null ? null : token.getBinder();
        return binder instanceof ControllerBinder ? ((ControllerBinder) binder).mSession : null;
    }

    public MediaSession(android.content.Context context, java.lang.String tag) { this(context, tag, null); }
    public MediaSession(android.content.Context context, java.lang.String tag, android.os.Bundle sessionInfo) {
        if (context == null) throw new IllegalArgumentException("context cannot be null.");
        if (tag == null || tag.isEmpty()) throw new IllegalArgumentException("tag cannot be null or empty");
        mTag = tag;
        mPackageName = context.getPackageName();
        mSessionInfo = sessionInfo == null ? null : new android.os.Bundle(sessionInfo);
        mToken = new Token(android.os.Process.myUid(), new ControllerBinder(this));
        mController = new MediaController(context, mToken);
    }
    protected void finalize() {}
    /** The package of the controller whose call the callback is in, null outside of one. */
    public java.lang.String getCallingPackage() {
        android.media.session.MediaSessionManager.RemoteUserInfo info = mCurrentControllerInfo;
        return info == null ? null : info.getPackageName();
    }
    public android.media.session.MediaController getController() { return mController; }
    /*
     * Who sent what the callback is being told, while it is (Android's MediaSession.Callback methods): a controller's
     * package, with the process's pid and uid (every controller is of this process), and for a key of the phone's media
     * controls the system's, as Android's media session service tells a session of a key it sent itself -- the package
     * "android", the system's uid. Outside of them Android throws, and so does this. It gave null, and Spotify, which asks
     * it in onPause, ended when the lock screen's pause was pressed (GitHub issue 4).
     */
    private android.media.session.MediaSessionManager.RemoteUserInfo mCurrentControllerInfo;
    public android.media.session.MediaSessionManager.RemoteUserInfo getCurrentControllerInfo() {
        android.media.session.MediaSessionManager.RemoteUserInfo info = mCurrentControllerInfo;
        if (info == null) throw new IllegalStateException("This should be called inside of MediaSession.Callback methods");
        return info;
    }
    public android.media.session.MediaSession.Token getSessionToken() { return mToken; }
    public boolean isActive() { return mActive; }
    /** A remote session's volume changed (its VolumeProvider says so itself, too): the controllers hear the new one. */
    public void notifyRemoteVolumeChanged(android.media.VolumeProvider provider) {
        if (provider == null || provider != mVolumeProvider) return;
        tellControllers(MediaController.MSG_UPDATE_VOLUME, playbackInfo(), null);
    }
    /** The session's end: no callback any more, and its controllers hear onSessionDestroyed. */
    public void release() {
        setActive(false);
        setCallback(null);
        if (mReleased) return;
        mReleased = true;
        tellControllers(MediaController.MSG_DESTROYED, null, null);
    }
    public void sendSessionEvent(java.lang.String event, android.os.Bundle extras) {
        if (event == null || event.isEmpty()) throw new IllegalArgumentException("event cannot be null or empty");
        tellControllers(MediaController.MSG_EVENT, event, extras);
    }
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
    public void setCallback(android.media.session.MediaSession.Callback callback) { setCallback(callback, null); }
    /**
     * The callback hears on the given handler's thread (this thread's, or the main one, with none), through a handler of
     * the session's own, as Android's: what waited for a callback taken away is dropped, and an app that clears its own
     * handler (Deezer's service does, removeCallbacksAndMessages(null)) loses nothing of the session's.
     */
    public void setCallback(android.media.session.MediaSession.Callback callback, android.os.Handler handler) {
        synchronized (mControllers) {
            Callback old = mCallback;
            if (old != null) old.mSession = null;
            if (callback == null) { mCallback = null; mHandler = null; return; }
            Looper looper = handler != null ? handler.getLooper() : Looper.myLooper();
            callback.mSession = this;
            mHandler = new Handler(looper != null ? looper : Looper.getMainLooper());
            mCallback = callback;
        }
    }
    public void setExtras(android.os.Bundle extras) { mExtras = extras; tellControllers(MediaController.MSG_UPDATE_EXTRAS, extras, null); }
    public void setFlags(int flags) { mFlags = flags; }
    public void setMediaButtonBroadcastReceiver(android.content.ComponentName receiver) {}
    public void setMediaButtonReceiver(android.app.PendingIntent receiver) {}
    public void setMetadata(android.media.MediaMetadata metadata) {
        mMetadata = metadata;
        tellControllers(MediaController.MSG_UPDATE_METADATA, metadata, null);
        if (isTop()) tellMetadata();
    }
    public void setPlaybackState(android.media.session.PlaybackState state) {
        mPlaybackState = state;
        tellControllers(MediaController.MSG_UPDATE_PLAYBACK_STATE, state, null);
        if (isTop()) tellState();
    }
    /** The session plays on the phone (as it does until told otherwise): its volume is the stream's of these attributes. */
    public void setPlaybackToLocal(android.media.AudioAttributes attributes) {
        if (attributes == null) throw new IllegalArgumentException("Attributes cannot be null for local playback.");
        boolean wasRemote = mVolumeProvider != null;
        mAudioAttributes = attributes;
        mVolumeProvider = null;
        if (wasRemote) tellControllers(MediaController.MSG_UPDATE_VOLUME, playbackInfo(), null);
    }
    /** The session plays elsewhere: its volume is the provider's, which a controller's setVolumeTo and adjustVolume reach. */
    public void setPlaybackToRemote(android.media.VolumeProvider provider) {
        if (provider == null) throw new IllegalArgumentException("volumeProvider may not be null!");
        mVolumeProvider = provider;
        provider.setCallback(new android.media.VolumeProvider.Callback() {
            @Override public void onVolumeChanged(android.media.VolumeProvider changed) { notifyRemoteVolumeChanged(changed); }
        });
        tellControllers(MediaController.MSG_UPDATE_VOLUME, playbackInfo(), null);
    }
    public void setQueue(java.util.List queue) {
        java.util.List<QueueItem> copy = queue == null ? null : new java.util.ArrayList<QueueItem>(queue);
        mQueue = copy;
        tellControllers(MediaController.MSG_UPDATE_QUEUE, copy == null ? null : new java.util.ArrayList<QueueItem>(copy), null);
    }
    public void setQueueTitle(java.lang.CharSequence title) { mQueueTitle = title; tellControllers(MediaController.MSG_UPDATE_QUEUE_TITLE, title, null); }
    public void setRatingType(int type) { mRatingType = type; }
    public void setSessionActivity(android.app.PendingIntent activity) { mSessionActivity = activity; }

    // ---- what a controller reads, hears and asks (MediaController.java)
    String tag() { return mTag; }
    String packageName() { return mPackageName; }
    android.os.Bundle sessionInfo() { return mSessionInfo; }
    boolean released() { return mReleased; }
    PlaybackState playbackState() { return mPlaybackState; }
    MediaMetadata metadata() { return mMetadata; }
    java.util.List<QueueItem> queue() { return mQueue; }
    CharSequence queueTitle() { return mQueueTitle; }
    android.os.Bundle extras() { return mExtras; }
    int flags() { return mFlags; }
    int ratingType() { return mRatingType; }
    android.app.PendingIntent sessionActivity() { return mSessionActivity; }
    android.media.VolumeProvider volumeProvider() { return mVolumeProvider; }
    /** Where the session plays and how loud: the provider's word for a remote one, the phone's stream for a local one. */
    MediaController.PlaybackInfo playbackInfo() {
        android.media.VolumeProvider provider = mVolumeProvider;
        android.media.AudioAttributes attributes = mAudioAttributes;
        if (attributes == null) attributes = new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_MEDIA).build();
        if (provider != null)
            return new MediaController.PlaybackInfo(MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE, provider.getVolumeControl(), provider.getMaxVolume(),
                    provider.getCurrentVolume(), attributes, provider.getVolumeControlId());
        android.media.AudioManager audio = new android.media.AudioManager();
        int stream = volumeStream();
        return new MediaController.PlaybackInfo(MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL, android.media.VolumeProvider.VOLUME_CONTROL_ABSOLUTE,
                audio.getStreamMaxVolume(stream), audio.getStreamVolume(stream), attributes, null);
    }
    /** The stream whose volume a local session's is: its attributes' own when they name one, else music's. */
    int volumeStream() {
        android.media.AudioAttributes attributes = mAudioAttributes;
        int stream = attributes == null ? -1 : attributes.getVolumeControlStream();
        return stream >= 0 ? stream : android.media.AudioManager.STREAM_MUSIC;
    }
    void addController(MediaController controller) { mControllers.addIfAbsent(controller); }
    void removeController(MediaController controller) { mControllers.remove(controller); }
    private void tellControllers(int what, Object obj, android.os.Bundle data) {
        for (MediaController controller : mControllers) controller.post(what, obj, data);
    }
    /** A call of the session's Callback, as one of a controller's reaches it. */
    interface Call {
        void to(Callback callback);
    }
    /**
     * A call for the callback, from whom: made later, on the callback's thread, with the sender there to be asked
     * (getCurrentControllerInfo) while it runs. False, and nothing is called, for a session with no callback.
     */
    boolean post(final android.media.session.MediaSessionManager.RemoteUserInfo from, final Call call) {
        final Callback callback;
        final Handler handler;
        synchronized (mControllers) { callback = mCallback; handler = mHandler; }
        if (callback == null || handler == null) return false;
        return handler.post(new Runnable() {
            public void run() {
                // (A callback taken away or changed since: what waited for it is dropped, as Android removes its messages.)
                if (mCallback != callback) return;
                android.media.session.MediaSessionManager.RemoteUserInfo was = mCurrentControllerInfo;
                mCurrentControllerInfo = from;
                try { call.to(callback); } finally { mCurrentControllerInfo = was; }
            }
        });
    }

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
        final Intent i = new Intent(Intent.ACTION_MEDIA_BUTTON);
        i.putExtra(Intent.EXTRA_KEY_EVENT, e);
        s.post(new android.media.session.MediaSessionManager.RemoteUserInfo("android", android.os.Process.myPid(), android.os.Process.SYSTEM_UID),
                new Call() {
                    public void to(Callback callback) { callback.onMediaButtonEvent(i); }
                });
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
    /** One of a session's queue: what it is, and its id in the queue (never UNKNOWN_ID). */
    public static final class QueueItem implements android.os.Parcelable {
        public static final int UNKNOWN_ID = -1;
        private final android.media.MediaDescription mDescription;
        private final long mId;
        public QueueItem(android.media.MediaDescription description, long id) {
            if (description == null) throw new IllegalArgumentException("Description cannot be null");
            if (id == UNKNOWN_ID) throw new IllegalArgumentException("Id cannot be QueueItem.UNKNOWN_ID");
            mDescription = description;
            mId = id;
        }
        public android.media.MediaDescription getDescription() { return mDescription; }
        public long getQueueId() { return mId; }
        public int describeContents() { return 0; }
        // (The description as the object it is: MediaDescription here writes nothing of itself into a Parcel.)
        public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeParcelable(mDescription, flags); dest.writeLong(mId); }
        public static final android.os.Parcelable.Creator<QueueItem> CREATOR = new android.os.Parcelable.Creator<QueueItem>() {
            public QueueItem createFromParcel(android.os.Parcel in) {
                android.media.MediaDescription description = in.readParcelable(QueueItem.class.getClassLoader());
                return new QueueItem(description, in.readLong());
            }
            public QueueItem[] newArray(int size) { return new QueueItem[size]; }
        };
        @Override public String toString() { return "MediaSession.QueueItem {Description=" + mDescription + ", Id=" + mId + " }"; }
        @Override public boolean equals(Object o) {
            if (o == null) return false;
            if (!(o instanceof QueueItem)) return false;
            QueueItem item = (QueueItem) o;
            return mId == item.mId && java.util.Objects.equals(mDescription, item.mDescription);
        }
    }
    /** What a controller of a session is made of: the session's binder, and the uid of its app. Equal for one session. */
    public static final class Token implements android.os.Parcelable {
        private final int mUid;
        private final android.media.session.ISessionController mBinder;
        public Token(int uid, android.media.session.ISessionController binder) { mUid = uid; mBinder = binder; }
        public int describeContents() { return 0; }
        public android.media.session.ISessionController getBinder() { return mBinder; }
        public int getUid() { return mUid; }
        public void writeToParcel(android.os.Parcel dest, int flags) {
            dest.writeInt(mUid);
            dest.writeStrongBinder(mBinder == null ? null : mBinder.asBinder());
        }
        public static final android.os.Parcelable.Creator<Token> CREATOR = new android.os.Parcelable.Creator<Token>() {
            public Token createFromParcel(android.os.Parcel in) {
                int uid = in.readInt();
                android.os.IBinder binder = in.readStrongBinder();
                return new Token(uid, binder instanceof ISessionController ? (ISessionController) binder : null);
            }
            public Token[] newArray(int size) { return new Token[size]; }
        };
        @Override public int hashCode() {
            return 31 * (31 + mUid) + (mBinder == null ? 0 : mBinder.asBinder().hashCode());
        }
        @Override public boolean equals(Object o) {
            if (this == o) return true;
            if (o == null || getClass() != o.getClass()) return false;
            Token other = (Token) o;
            if (mUid != other.mUid) return false;
            if (mBinder == null || other.mBinder == null) return mBinder == other.mBinder;
            return java.util.Objects.equals(mBinder.asBinder(), other.mBinder.asBinder());
        }
    }
}
