package android.media.session;

import android.content.Context;
import android.content.Intent;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.VolumeProvider;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.List;

/**
 * Android's MediaController, of a session of this process (MediaSession.getSessionToken(), which is all an app can have
 * here: no other app's sessions are to be seen). It reads what the session was given, tells the callbacks registered with
 * it of each change -- each on the thread of the handler it was registered with -- and what it is asked reaches the
 * session's Callback as MediaSession.post makes such a call: later, on the session's callback thread, from this
 * controller's package. A token of no session here (none can be made by an app) gives a controller that reads nothing and
 * asks nobody.
 *
 * It was a stub: no transport controls (null), no callbacks, nothing read. androidx's MediaControllerCompat is made of
 * one, and an app that drives its own playback service through it could not start playing: Deezer's play button is
 * getTransportControls().play() (0.1.0.690).
 */
@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaController {
    /* What a callback is told, as its handler's messages (MediaSession tells them through post). */
    static final int MSG_EVENT = 1;
    static final int MSG_UPDATE_PLAYBACK_STATE = 2;
    static final int MSG_UPDATE_METADATA = 3;
    static final int MSG_UPDATE_VOLUME = 4;
    static final int MSG_UPDATE_QUEUE = 5;
    static final int MSG_UPDATE_QUEUE_TITLE = 6;
    static final int MSG_UPDATE_EXTRAS = 7;
    static final int MSG_DESTROYED = 8;

    private final Context mContext;
    private final MediaSession.Token mToken;
    /** The session the token is of; null for a token of none of this process's. */
    private final MediaSession mSession;
    private final TransportControls mTransportControls;
    /** The callbacks registered, each with its handler; their lock. */
    private final ArrayList<MessageHandler> mCallbacks = new ArrayList<>();
    private boolean mCbRegistered;

    public MediaController(Context context, MediaSession.Token token) {
        if (context == null) throw new IllegalArgumentException("context shouldn't be null");
        if (token == null) throw new IllegalArgumentException("token shouldn't be null");
        if (token.getBinder() == null) throw new IllegalArgumentException("token.getBinder() shouldn't be null");
        mContext = context;
        mToken = token;
        mSession = MediaSession.of(token);
        mTransportControls = new TransportControls(this);
    }

    /** Who asks, for the session's getCurrentControllerInfo: this controller's package, in this process. */
    private MediaSessionManager.RemoteUserInfo from() {
        return new MediaSessionManager.RemoteUserInfo(mContext.getPackageName(), android.os.Process.myPid(), android.os.Process.myUid());
    }
    /** A call of the session's Callback, made later on its thread; nothing for a session gone or with no callback. */
    private boolean ask(MediaSession.Call call) {
        MediaSession session = mSession;
        return session != null && session.post(from(), call);
    }

    public TransportControls getTransportControls() { return mTransportControls; }

    /** A media key for the session's Callback.onMediaButtonEvent; false for any other key, or with nobody to hear it. */
    public boolean dispatchMediaButtonEvent(KeyEvent keyEvent) {
        if (keyEvent == null) throw new IllegalArgumentException("KeyEvent may not be null");
        if (!KeyEvent.isMediaSessionKey(keyEvent.getKeyCode())) return false;
        final Intent intent = new Intent(Intent.ACTION_MEDIA_BUTTON);
        intent.putExtra(Intent.EXTRA_KEY_EVENT, keyEvent);
        return ask(callback -> callback.onMediaButtonEvent(intent));
    }

    /**
     * The session's state, its position brought to now while it plays (or seeks) and says when the position was taken, no
     * further than the metadata's duration; null with none set, or once the session is released.
     */
    public PlaybackState getPlaybackState() {
        MediaSession session = mSession;
        if (session == null || session.released()) return null;
        PlaybackState state = session.playbackState();
        if (state == null) return null;
        int s = state.getState();
        if (s != PlaybackState.STATE_PLAYING && s != PlaybackState.STATE_FAST_FORWARDING && s != PlaybackState.STATE_REWINDING) return state;
        long updateTime = state.getLastPositionUpdateTime();
        if (updateTime <= 0) return state;
        long now = android.os.SystemClock.elapsedRealtime();
        long position = (long) (state.getPlaybackSpeed() * (now - updateTime)) + state.getPosition();
        MediaMetadata metadata = session.metadata();
        long duration = metadata != null && metadata.containsKey(MediaMetadata.METADATA_KEY_DURATION)
                ? metadata.getLong(MediaMetadata.METADATA_KEY_DURATION) : -1;
        if (duration >= 0 && position > duration) position = duration;
        else if (position < 0) position = 0;
        return new PlaybackState.Builder(state).setState(s, position, state.getPlaybackSpeed(), now).build();
    }
    public MediaMetadata getMetadata() { MediaSession s = mSession; return s == null ? null : s.metadata(); }
    /** The session's queue, a list of the caller's own; null with none set. */
    public List<MediaSession.QueueItem> getQueue() {
        MediaSession s = mSession;
        List<MediaSession.QueueItem> queue = s == null ? null : s.queue();
        return queue == null ? null : new ArrayList<MediaSession.QueueItem>(queue);
    }
    public CharSequence getQueueTitle() { MediaSession s = mSession; return s == null ? null : s.queueTitle(); }
    public Bundle getExtras() {
        MediaSession s = mSession;
        Bundle extras = s == null ? null : s.extras();
        return extras == null ? null : new Bundle(extras);
    }
    public int getRatingType() { MediaSession s = mSession; return s == null ? android.media.Rating.RATING_NONE : s.ratingType(); }
    public long getFlags() { MediaSession s = mSession; return s == null ? 0 : s.flags(); }
    /** Where the session plays and how loud: the phone's stream, or what its VolumeProvider says. */
    public PlaybackInfo getPlaybackInfo() { MediaSession s = mSession; return s == null ? null : s.playbackInfo(); }
    public android.app.PendingIntent getSessionActivity() { MediaSession s = mSession; return s == null ? null : s.sessionActivity(); }
    public MediaSession.Token getSessionToken() { return mToken; }
    public String getPackageName() { MediaSession s = mSession; return s == null ? null : s.packageName(); }
    public String getTag() { MediaSession s = mSession; return s == null ? null : s.tag(); }
    /** What the session was made with (a Bundle of the caller's own); an empty one with nothing. */
    public Bundle getSessionInfo() {
        MediaSession s = mSession;
        Bundle info = s == null ? null : s.sessionInfo();
        return new Bundle(info == null ? Bundle.EMPTY : info);
    }
    public boolean controlsSameSession(MediaController other) { return other != null && mToken.equals(other.mToken); }

    /**
     * A local session's volume is its stream's, set there; a remote one's is its VolumeProvider's to set, asked on the
     * session's callback thread -- when the provider takes absolute volumes, clamped to its range.
     */
    public void setVolumeTo(int value, int flags) {
        MediaSession session = mSession;
        if (session == null) return;
        final VolumeProvider provider = session.volumeProvider();
        if (provider == null) {
            new AudioManager().setStreamVolume(session.volumeStream(), value, flags);
            return;
        }
        if (provider.getVolumeControl() != VolumeProvider.VOLUME_CONTROL_ABSOLUTE) return;
        final int volume = Math.max(0, Math.min(value, provider.getMaxVolume()));
        ask(callback -> provider.onSetVolumeTo(volume));
    }
    /** A step of the volume: the stream's for a local session, the VolumeProvider's own for a remote one (not a fixed one). */
    public void adjustVolume(final int direction, int flags) {
        MediaSession session = mSession;
        if (session == null) return;
        final VolumeProvider provider = session.volumeProvider();
        if (provider == null) {
            new AudioManager().adjustStreamVolume(session.volumeStream(), direction, flags);
            return;
        }
        if (provider.getVolumeControl() == VolumeProvider.VOLUME_CONTROL_FIXED) return;
        // (Muting a remote session is not a thing Android does either.)
        if (direction == AudioManager.ADJUST_TOGGLE_MUTE || direction == AudioManager.ADJUST_MUTE || direction == AudioManager.ADJUST_UNMUTE) return;
        ask(callback -> provider.onAdjustVolume(direction));
    }

    public void registerCallback(Callback callback) { registerCallback(callback, null); }
    /**
     * The callback hears of the session's changes on the handler's thread (this thread's, with none). One registered
     * already stays as it is. For a session released already it hears onSessionDestroyed, and that is all.
     */
    public void registerCallback(Callback callback, Handler handler) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        if (handler == null) handler = new Handler();
        synchronized (mCallbacks) {
            if (holder(callback) != null) return;
            MessageHandler holder = new MessageHandler(handler.getLooper(), callback);
            mCallbacks.add(holder);
            holder.mRegistered = true;
            if (!mCbRegistered && mSession != null) {
                mCbRegistered = true;
                mSession.addController(this);
                if (mSession.released()) post(MSG_DESTROYED, null, null);
            }
        }
    }
    public void unregisterCallback(Callback callback) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        synchronized (mCallbacks) {
            for (int i = mCallbacks.size() - 1; i >= 0; i--) {
                MessageHandler holder = mCallbacks.get(i);
                if (holder.mCallback == callback) {
                    mCallbacks.remove(i);
                    holder.mRegistered = false;
                }
            }
            if (mCbRegistered && mCallbacks.isEmpty()) {
                mSession.removeController(this);
                mCbRegistered = false;
            }
        }
    }
    /** The handler a callback hears on, null for one not registered. */
    public Handler getHandlerForCallback(Callback callback) {
        synchronized (mCallbacks) { return holder(callback); }
    }
    private MessageHandler holder(Callback callback) {
        if (callback == null) throw new IllegalArgumentException("Callback cannot be null");
        for (int i = mCallbacks.size() - 1; i >= 0; i--) if (mCallbacks.get(i).mCallback == callback) return mCallbacks.get(i);
        return null;
    }
    /** The session says something changed: each callback hears it, later, on its own thread. */
    void post(int what, Object obj, Bundle data) {
        synchronized (mCallbacks) {
            for (int i = mCallbacks.size() - 1; i >= 0; i--) mCallbacks.get(i).post(what, obj, data);
        }
    }

    /** A command of the app's own for the session's Callback.onCommand, which may answer through the receiver. */
    public void sendCommand(final String command, final Bundle args, final android.os.ResultReceiver cb) {
        if (command == null || command.isEmpty()) throw new IllegalArgumentException("command cannot be null or empty");
        ask(callback -> callback.onCommand(command, args, cb));
    }

    /** One registered callback on its thread; what waits for a callback unregistered since is dropped. */
    private static final class MessageHandler extends Handler {
        // (Named in full: within a Handler, "Callback" is Handler.Callback.)
        final MediaController.Callback mCallback;
        volatile boolean mRegistered;
        MessageHandler(Looper looper, MediaController.Callback callback) {
            super(looper);
            mCallback = callback;
        }
        @Override public void handleMessage(Message msg) {
            if (!mRegistered) return;
            switch (msg.what) {
                case MSG_EVENT: mCallback.onSessionEvent((String) msg.obj, msg.getData()); break;
                case MSG_UPDATE_PLAYBACK_STATE: mCallback.onPlaybackStateChanged((PlaybackState) msg.obj); break;
                case MSG_UPDATE_METADATA: mCallback.onMetadataChanged((MediaMetadata) msg.obj); break;
                case MSG_UPDATE_QUEUE: mCallback.onQueueChanged((List) msg.obj); break;
                case MSG_UPDATE_QUEUE_TITLE: mCallback.onQueueTitleChanged((CharSequence) msg.obj); break;
                case MSG_UPDATE_EXTRAS: mCallback.onExtrasChanged((Bundle) msg.obj); break;
                case MSG_UPDATE_VOLUME: mCallback.onAudioInfoChanged((PlaybackInfo) msg.obj); break;
                case MSG_DESTROYED: mCallback.onSessionDestroyed(); break;
            }
        }
        void post(int what, Object obj, Bundle data) {
            Message msg = obtainMessage(what, obj);
            msg.setData(data);
            msg.sendToTarget();
        }
    }

    public static abstract class Callback {
        public Callback() {}
        public void onAudioInfoChanged(android.media.session.MediaController.PlaybackInfo info) {}
        public void onExtrasChanged(android.os.Bundle extras) {}
        public void onMetadataChanged(android.media.MediaMetadata metadata) {}
        public void onPlaybackStateChanged(android.media.session.PlaybackState state) {}
        public void onQueueChanged(java.util.List queue) {}
        public void onQueueTitleChanged(java.lang.CharSequence title) {}
        public void onSessionDestroyed() {}
        public void onSessionEvent(java.lang.String event, android.os.Bundle extras) {}
    }

    /** Where a session plays -- on the phone or elsewhere -- and its volume: the kind of control, the range, the level. */
    public static final class PlaybackInfo implements android.os.Parcelable {
        public static final int PLAYBACK_TYPE_LOCAL = 1;
        public static final int PLAYBACK_TYPE_REMOTE = 2;
        private final int mPlaybackType, mVolumeControl, mMaxVolume, mCurrentVolume;
        private final android.media.AudioAttributes mAudioAttrs;
        private final String mVolumeControlId;
        public PlaybackInfo(int playbackType, int volumeControl, int maxVolume, int currentVolume, android.media.AudioAttributes audioAttrs,
                java.lang.String volumeControlId) {
            mPlaybackType = playbackType;
            mVolumeControl = volumeControl;
            mMaxVolume = maxVolume;
            mCurrentVolume = currentVolume;
            mAudioAttrs = audioAttrs;
            mVolumeControlId = volumeControlId;
        }
        public int describeContents() { return 0; }
        public android.media.AudioAttributes getAudioAttributes() { return mAudioAttrs; }
        public int getCurrentVolume() { return mCurrentVolume; }
        public int getMaxVolume() { return mMaxVolume; }
        public int getPlaybackType() { return mPlaybackType; }
        public int getVolumeControl() { return mVolumeControl; }
        public java.lang.String getVolumeControlId() { return mVolumeControlId; }
        @Override public String toString() {
            return "playbackType=" + mPlaybackType + ", volumeControlType=" + mVolumeControl + ", maxVolume=" + mMaxVolume
                    + ", currentVolume=" + mCurrentVolume + ", audioAttrs=" + mAudioAttrs + ", volumeControlId=" + mVolumeControlId;
        }
        public void writeToParcel(android.os.Parcel dest, int flags) {
            dest.writeInt(mPlaybackType);
            dest.writeInt(mVolumeControl);
            dest.writeInt(mMaxVolume);
            dest.writeInt(mCurrentVolume);
            dest.writeParcelable(mAudioAttrs, flags);
            dest.writeString(mVolumeControlId);
        }
        public static final android.os.Parcelable.Creator<PlaybackInfo> CREATOR = new android.os.Parcelable.Creator<PlaybackInfo>() {
            public PlaybackInfo createFromParcel(android.os.Parcel in) {
                int type = in.readInt(), control = in.readInt(), max = in.readInt(), current = in.readInt();
                android.media.AudioAttributes attributes = in.readParcelable(null);
                return new PlaybackInfo(type, control, max, current, attributes, in.readString());
            }
            public PlaybackInfo[] newArray(int size) { return new PlaybackInfo[size]; }
        };
    }

    /**
     * What a controller asks of its session's playback: each a call of the session's Callback, made later on the callback's
     * thread (play() is Callback.onPlay(), and so on), with Android's refusals of what names nothing.
     */
    public static final class TransportControls {
        private final MediaController mController;
        TransportControls(MediaController controller) { mController = controller; }
        public void fastForward() { mController.ask(callback -> callback.onFastForward()); }
        public void pause() { mController.ask(callback -> callback.onPause()); }
        public void play() { mController.ask(callback -> callback.onPlay()); }
        public void playFromMediaId(final java.lang.String mediaId, final android.os.Bundle extras) {
            if (mediaId == null || mediaId.isEmpty()) throw new IllegalArgumentException("You must specify a non-empty String for playFromMediaId.");
            mController.ask(callback -> callback.onPlayFromMediaId(mediaId, extras));
        }
        public void playFromSearch(java.lang.String query, final android.os.Bundle extras) {
            // (No query is the empty one, as the search intent's is.)
            final String q = query == null ? "" : query;
            mController.ask(callback -> callback.onPlayFromSearch(q, extras));
        }
        public void playFromUri(final android.net.Uri uri, final android.os.Bundle extras) {
            if (uri == null || android.net.Uri.EMPTY.equals(uri)) throw new IllegalArgumentException("You must specify a non-empty Uri for playFromUri.");
            mController.ask(callback -> callback.onPlayFromUri(uri, extras));
        }
        public void prepare() { mController.ask(callback -> callback.onPrepare()); }
        public void prepareFromMediaId(final java.lang.String mediaId, final android.os.Bundle extras) {
            if (mediaId == null || mediaId.isEmpty()) throw new IllegalArgumentException("You must specify a non-empty String for prepareFromMediaId.");
            mController.ask(callback -> callback.onPrepareFromMediaId(mediaId, extras));
        }
        public void prepareFromSearch(java.lang.String query, final android.os.Bundle extras) {
            final String q = query == null ? "" : query;
            mController.ask(callback -> callback.onPrepareFromSearch(q, extras));
        }
        public void prepareFromUri(final android.net.Uri uri, final android.os.Bundle extras) {
            if (uri == null || android.net.Uri.EMPTY.equals(uri)) throw new IllegalArgumentException("You must specify a non-empty Uri for prepareFromUri.");
            mController.ask(callback -> callback.onPrepareFromUri(uri, extras));
        }
        public void rewind() { mController.ask(callback -> callback.onRewind()); }
        public void seekTo(final long pos) { mController.ask(callback -> callback.onSeekTo(pos)); }
        public void sendCustomAction(android.media.session.PlaybackState.CustomAction customAction, android.os.Bundle args) {
            if (customAction == null) throw new IllegalArgumentException("CustomAction cannot be null.");
            sendCustomAction(customAction.getAction(), args);
        }
        public void sendCustomAction(final java.lang.String action, final android.os.Bundle args) {
            if (action == null || action.isEmpty()) throw new IllegalArgumentException("CustomAction cannot be null.");
            mController.ask(callback -> callback.onCustomAction(action, args));
        }
        public void setPlaybackSpeed(final float speed) {
            if (speed == 0.0f) throw new IllegalArgumentException("speed must not be zero");
            mController.ask(callback -> callback.onSetPlaybackSpeed(speed));
        }
        public void setRating(final android.media.Rating rating) { mController.ask(callback -> callback.onSetRating(rating)); }
        public void skipToNext() { mController.ask(callback -> callback.onSkipToNext()); }
        public void skipToPrevious() { mController.ask(callback -> callback.onSkipToPrevious()); }
        public void skipToQueueItem(final long id) { mController.ask(callback -> callback.onSkipToQueueItem(id)); }
        public void stop() { mController.ask(callback -> callback.onStop()); }
    }
}
