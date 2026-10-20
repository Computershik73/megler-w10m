// Overlay: android.media.session.PlaybackState, in place of the generated stub (core/framework/gen-src).
//
// Why. The stub kept almost nothing: setState and addCustomAction were dropped, so the state, the position and the speed
// read back as 0; CREATOR was null; and CustomAction was an empty abstract class without its Builder. AndroidX's
// PlaybackStateCompat.getPlaybackState() makes the platform's object with PlaybackState.Builder and, for each custom
// action, PlaybackState.CustomAction.Builder; MediaSessionCompat does that for every setPlaybackState. Spotify's
// "MediaSession worker" thread ended on NoClassDefFoundError: android/media/session/PlaybackState$CustomAction$Builder.
//
// This is Android's class, as AOSP has it: the state, the position at the time it was last updated and the speed it moves
// at, the buffered position, the actions, the custom actions, the active queue item, the error message and the extras;
// its Builder; its Parcel form and CREATOR; CustomAction with its own Builder, Parcel form and CREATOR. isActive is
// Android's list of the states that are playing or about to.
package android.media.session;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;

public final class PlaybackState implements Parcelable {
    public static final long ACTION_STOP = 1L;
    public static final long ACTION_PAUSE = 2L;
    public static final long ACTION_PLAY = 4L;
    public static final long ACTION_REWIND = 8L;
    public static final long ACTION_SKIP_TO_PREVIOUS = 16L;
    public static final long ACTION_SKIP_TO_NEXT = 32L;
    public static final long ACTION_FAST_FORWARD = 64L;
    public static final long ACTION_SET_RATING = 128L;
    public static final long ACTION_SEEK_TO = 256L;
    public static final long ACTION_PLAY_PAUSE = 512L;
    public static final long ACTION_PLAY_FROM_MEDIA_ID = 1024L;
    public static final long ACTION_PLAY_FROM_SEARCH = 2048L;
    public static final long ACTION_SKIP_TO_QUEUE_ITEM = 4096L;
    public static final long ACTION_PLAY_FROM_URI = 8192L;
    public static final long ACTION_PREPARE = 16384L;
    public static final long ACTION_PREPARE_FROM_MEDIA_ID = 32768L;
    public static final long ACTION_PREPARE_FROM_SEARCH = 65536L;
    public static final long ACTION_PREPARE_FROM_URI = 131072L;
    public static final long ACTION_SET_PLAYBACK_SPEED = 4194304L;
    public static final long PLAYBACK_POSITION_UNKNOWN = -1L;
    public static final int STATE_NONE = 0;
    public static final int STATE_STOPPED = 1;
    public static final int STATE_PAUSED = 2;
    public static final int STATE_PLAYING = 3;
    public static final int STATE_FAST_FORWARDING = 4;
    public static final int STATE_REWINDING = 5;
    public static final int STATE_BUFFERING = 6;
    public static final int STATE_ERROR = 7;
    public static final int STATE_CONNECTING = 8;
    public static final int STATE_SKIPPING_TO_PREVIOUS = 9;
    public static final int STATE_SKIPPING_TO_NEXT = 10;
    public static final int STATE_SKIPPING_TO_QUEUE_ITEM = 11;

    private final int mState;
    private final long mPosition;
    private final long mBufferedPosition;
    private final float mSpeed;
    private final long mActions;
    private final List<CustomAction> mCustomActions;
    private final CharSequence mErrorMessage;
    private final long mUpdateTime;
    private final long mActiveItemId;
    private final Bundle mExtras;

    private PlaybackState(int state, long position, long updateTime, float speed, long bufferedPosition, long actions,
            List<CustomAction> customActions, long activeItemId, CharSequence error, Bundle extras) {
        mState = state;
        mPosition = position;
        mSpeed = speed;
        mUpdateTime = updateTime;
        mBufferedPosition = bufferedPosition;
        mActions = actions;
        mCustomActions = new ArrayList<>(customActions);
        mActiveItemId = activeItemId;
        mErrorMessage = error;
        mExtras = extras;
    }

    private PlaybackState(Parcel in) {
        mState = in.readInt();
        mPosition = in.readLong();
        mSpeed = in.readFloat();
        mUpdateTime = in.readLong();
        mBufferedPosition = in.readLong();
        mActions = in.readLong();
        int n = in.readInt();
        mCustomActions = new ArrayList<>(Math.max(n, 0));
        for (int i = 0; i < n; i++) mCustomActions.add(CustomAction.CREATOR.createFromParcel(in));
        mActiveItemId = in.readLong();
        mErrorMessage = in.readCharSequence();
        mExtras = in.readBundle();
    }

    public int getState() { return mState; }

    public long getPosition() { return mPosition; }

    public long getBufferedPosition() { return mBufferedPosition; }

    public float getPlaybackSpeed() { return mSpeed; }

    public long getActions() { return mActions; }

    public List<CustomAction> getCustomActions() { return mCustomActions; }

    public CharSequence getErrorMessage() { return mErrorMessage; }

    public long getLastPositionUpdateTime() { return mUpdateTime; }

    public long getActiveQueueItemId() { return mActiveItemId; }

    public Bundle getExtras() { return mExtras; }

    /** Whether the state is one of playing, or about to be. */
    public boolean isActive() {
        switch (mState) {
        case STATE_FAST_FORWARDING:
        case STATE_REWINDING:
        case STATE_SKIPPING_TO_PREVIOUS:
        case STATE_SKIPPING_TO_NEXT:
        case STATE_SKIPPING_TO_QUEUE_ITEM:
        case STATE_BUFFERING:
        case STATE_CONNECTING:
        case STATE_PLAYING:
            return true;
        default:
            return false;
        }
    }

    @Override
    public String toString() {
        return "PlaybackState {state=" + mState + ", position=" + mPosition + ", buffered position=" + mBufferedPosition + ", speed=" + mSpeed
                + ", updated=" + mUpdateTime + ", actions=" + mActions + ", custom actions=" + mCustomActions + ", active item id=" + mActiveItemId
                + ", error=" + mErrorMessage + "}";
    }

    @Override
    public int describeContents() { return 0; }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mState);
        dest.writeLong(mPosition);
        dest.writeFloat(mSpeed);
        dest.writeLong(mUpdateTime);
        dest.writeLong(mBufferedPosition);
        dest.writeLong(mActions);
        dest.writeInt(mCustomActions.size());
        for (CustomAction a : mCustomActions) a.writeToParcel(dest, flags);
        dest.writeLong(mActiveItemId);
        dest.writeCharSequence(mErrorMessage);
        dest.writeBundle(mExtras);
    }

    public static final Parcelable.Creator<PlaybackState> CREATOR = new Parcelable.Creator<PlaybackState>() {
        @Override
        public PlaybackState createFromParcel(Parcel in) { return new PlaybackState(in); }

        @Override
        public PlaybackState[] newArray(int size) { return new PlaybackState[size]; }
    };

    /** An action of the app's own, with its name and icon, beside the standard ones. */
    public static final class CustomAction implements Parcelable {
        private final String mAction;
        private final CharSequence mName;
        private final int mIcon;
        private final Bundle mExtras;

        private CustomAction(String action, CharSequence name, int icon, Bundle extras) {
            mAction = action;
            mName = name;
            mIcon = icon;
            mExtras = extras;
        }

        private CustomAction(Parcel in) {
            mAction = in.readString();
            mName = in.readCharSequence();
            mIcon = in.readInt();
            mExtras = in.readBundle();
        }

        public String getAction() { return mAction; }

        public CharSequence getName() { return mName; }

        public int getIcon() { return mIcon; }

        public Bundle getExtras() { return mExtras; }

        @Override
        public int describeContents() { return 0; }

        @Override
        public void writeToParcel(Parcel dest, int flags) {
            dest.writeString(mAction);
            dest.writeCharSequence(mName);
            dest.writeInt(mIcon);
            dest.writeBundle(mExtras);
        }

        @Override
        public String toString() { return "Action:mName='" + mName + ", mIcon=" + mIcon + ", mExtras=" + mExtras; }

        public static final Parcelable.Creator<CustomAction> CREATOR = new Parcelable.Creator<CustomAction>() {
            @Override
            public CustomAction createFromParcel(Parcel in) { return new CustomAction(in); }

            @Override
            public CustomAction[] newArray(int size) { return new CustomAction[size]; }
        };

        public static final class Builder {
            private final String mAction;
            private final CharSequence mName;
            private final int mIcon;
            private Bundle mExtras;

            public Builder(String action, CharSequence name, int icon) {
                if (action == null || action.isEmpty()) throw new IllegalArgumentException("You must specify an action to build a CustomAction.");
                if (name == null || name.length() == 0) throw new IllegalArgumentException("You must specify a name to build a CustomAction.");
                if (icon == 0) throw new IllegalArgumentException("You must specify an icon resource id to build a CustomAction.");
                mAction = action;
                mName = name;
                mIcon = icon;
            }

            public Builder setExtras(Bundle extras) {
                mExtras = extras;
                return this;
            }

            public CustomAction build() { return new CustomAction(mAction, mName, mIcon, mExtras); }
        }
    }

    public static final class Builder {
        private final List<CustomAction> mCustomActions = new ArrayList<>();
        private int mState;
        private long mPosition;
        private long mBufferedPosition;
        private float mSpeed;
        private long mActions;
        private CharSequence mErrorMessage;
        private long mUpdateTime;
        private long mActiveItemId = -1;  // MediaSession.QueueItem.UNKNOWN_ID
        private Bundle mExtras;

        public Builder() {}

        public Builder(PlaybackState from) {
            if (from == null) return;
            mState = from.mState;
            mPosition = from.mPosition;
            mBufferedPosition = from.mBufferedPosition;
            mSpeed = from.mSpeed;
            mActions = from.mActions;
            mCustomActions.addAll(from.mCustomActions);
            mErrorMessage = from.mErrorMessage;
            mUpdateTime = from.mUpdateTime;
            mActiveItemId = from.mActiveItemId;
            mExtras = from.mExtras;
        }

        public Builder setState(int state, long position, float playbackSpeed, long updateTime) {
            mState = state;
            mPosition = position;
            mUpdateTime = updateTime;
            mSpeed = playbackSpeed;
            return this;
        }

        public Builder setState(int state, long position, float playbackSpeed) {
            return setState(state, position, playbackSpeed, SystemClock.elapsedRealtime());
        }

        public Builder setActions(long actions) {
            mActions = actions;
            return this;
        }

        public Builder addCustomAction(String action, String name, int icon) {
            return addCustomAction(new CustomAction(action, name, icon, null));
        }

        public Builder addCustomAction(CustomAction customAction) {
            if (customAction == null) throw new IllegalArgumentException("You may not add a null CustomAction to PlaybackState.");
            mCustomActions.add(customAction);
            return this;
        }

        public Builder setBufferedPosition(long bufferedPosition) {
            mBufferedPosition = bufferedPosition;
            return this;
        }

        public Builder setActiveQueueItemId(long id) {
            mActiveItemId = id;
            return this;
        }

        public Builder setErrorMessage(CharSequence error) {
            mErrorMessage = error;
            return this;
        }

        public Builder setExtras(Bundle extras) {
            mExtras = extras;
            return this;
        }

        public PlaybackState build() {
            return new PlaybackState(mState, mPosition, mUpdateTime, mSpeed, mBufferedPosition, mActions, mCustomActions, mActiveItemId, mErrorMessage,
                    mExtras);
        }
    }
}
