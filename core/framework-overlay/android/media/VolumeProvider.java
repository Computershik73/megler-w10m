// Overlay of core/framework/gen-src/android/media/VolumeProvider.java (tools/build-framework.sh).
// It differs in keeping what it is made with. The generated stub's constructors dropped the kind of volume control, the
// maximum, the volume and the id, so each read back as 0 or null; here they are Android's: read as given, the volume
// changed by setCurrentVolume, which tells the callback set (the session's whose playback it is: MediaSession's
// setPlaybackToRemote sets one, and its controllers hear of the new volume).
//
// Why. A session that plays elsewhere -- a cast, a speaker -- hands one to MediaSession.setPlaybackToRemote, and a
// MediaController's getPlaybackInfo, setVolumeTo and adjustVolume are answered from it. The members are the stub's.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public abstract class VolumeProvider {
    public static final int VOLUME_CONTROL_ABSOLUTE = 2;
    public static final int VOLUME_CONTROL_FIXED = 0;
    public static final int VOLUME_CONTROL_RELATIVE = 1;

    private final int mControlType;
    private final int mMaxVolume;
    private final java.lang.String mControlId;
    private int mCurrentVolume;
    private Callback mCallback;

    public VolumeProvider(int volumeControl, int maxVolume, int currentVolume) { this(volumeControl, maxVolume, currentVolume, null); }
    public VolumeProvider(int volumeControl, int maxVolume, int currentVolume, java.lang.String volumeControlId) {
        mControlType = volumeControl;
        mMaxVolume = maxVolume;
        mCurrentVolume = currentVolume;
        mControlId = volumeControlId;
    }
    public final int getCurrentVolume() { return mCurrentVolume; }
    public final int getMaxVolume() { return mMaxVolume; }
    public final int getVolumeControl() { return mControlType; }
    public final java.lang.String getVolumeControlId() { return mControlId; }
    /** A controller asks for a step up (1), down (-1) or none (0): the provider's own to answer, with setCurrentVolume. */
    public void onAdjustVolume(int direction) {}
    /** A controller asks for this volume: the provider's own to answer, with setCurrentVolume. */
    public void onSetVolumeTo(int volume) {}
    public void setCallback(android.media.VolumeProvider.Callback callback) { mCallback = callback; }
    /** The volume is this now; whoever set a callback hears of it. */
    public final void setCurrentVolume(int currentVolume) {
        mCurrentVolume = currentVolume;
        Callback callback = mCallback;
        if (callback != null) callback.onVolumeChanged(this);
    }
    VolumeProvider() { this((int) 0, (int) 0, (int) 0); }
    public static abstract class Callback {
        public Callback() {}
        public abstract void onVolumeChanged(android.media.VolumeProvider volumeProvider);
    }
}
