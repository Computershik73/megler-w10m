// Overlay: MediaPlayer-backed video with Surface lifecycle and deferred start/seek.
package android.widget;
import android.content.Context;
import android.media.MediaPlayer;
import android.net.Uri;
import android.util.AttributeSet;
import android.view.SurfaceView;
import android.view.SurfaceHolder;

public class VideoView extends SurfaceView implements MediaController.MediaPlayerControl {
    private Uri mUri;
    private java.util.Map<String,String> mHeaders;
    private MediaPlayer mPlayer;
    private MediaController mController;
    private boolean mPrepared, mStart, mSurfaceReady;
    private int mSeek;
    private MediaPlayer.OnErrorListener mError;
    private MediaPlayer.OnPreparedListener mOnPrepared;
    private MediaPlayer.OnCompletionListener mCompletion;
    private MediaPlayer.OnInfoListener mInfo;
    public VideoView(Context c) { this(c, null); }
    public VideoView(Context c, AttributeSet a) { this(c, a, 0); }
    public VideoView(Context c, AttributeSet a, int s) { super(c, a, s); init(); }
    public VideoView(Context c, AttributeSet a, int s, int r) { super(c, a, s, r); init(); }
    private void init() {
        getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) { mSurfaceReady = true; open(); }
            public void surfaceChanged(SurfaceHolder h, int f, int w, int ht) {}
            public void surfaceDestroyed(SurfaceHolder h) {
                mSurfaceReady = false;
                if (mPrepared) mSeek = mPlayer.getCurrentPosition();
                close();
            }
        });
    }
    private void close() { if (mPlayer != null) mPlayer.release(); mPlayer = null; mPrepared = false; }
    private void open() {
        if (!mSurfaceReady || mUri == null) return;
        close();
        final MediaPlayer p = new MediaPlayer(); mPlayer = p;
        p.setOnPreparedListener(mp -> {
            if (mPlayer != mp) return;
            mPrepared = true;
            if (mSeek > 0) { mp.seekTo(mSeek); mSeek = 0; }
            requestLayout();
            if (mController != null) mController.setEnabled(true);
            if (mOnPrepared != null) mOnPrepared.onPrepared(mp);
            if (mStart) mp.start();
        });
        p.setOnCompletionListener(mp -> { mStart = false; if (mCompletion != null) mCompletion.onCompletion(mp); });
        p.setOnInfoListener((mp,w,e) -> mInfo != null && mInfo.onInfo(mp,w,e));
        p.setOnErrorListener((mp,w,e) -> { mStart = false; return mError != null && mError.onError(mp,w,e); });
        try { p.setDataSource(getContext(), mUri, mHeaders); p.setDisplay(getHolder()); p.prepareAsync(); }
        catch (java.io.IOException | IllegalArgumentException e) {
            post(() -> { if (mPlayer == p && mError != null) mError.onError(p, MediaPlayer.MEDIA_ERROR_UNKNOWN, MediaPlayer.MEDIA_ERROR_IO); });
        }
    }
    public void setVideoPath(String p) { setVideoURI(Uri.parse(p)); }
    public void setVideoURI(Uri u) { setVideoURI(u, null); }
    public void setVideoURI(Uri u, java.util.Map<String,String> headers) { mUri = u; mHeaders = headers == null ? null : new java.util.HashMap<>(headers); mSeek = 0; open(); requestLayout(); }
    public void setMediaController(MediaController c) { mController = c; if (c != null) { c.setMediaPlayer(this); c.setAnchorView(this); c.setEnabled(mPrepared); } }
    public void setOnPreparedListener(MediaPlayer.OnPreparedListener l) { mOnPrepared = l; }
    public void setOnCompletionListener(MediaPlayer.OnCompletionListener l) { mCompletion = l; }
    public void setOnErrorListener(MediaPlayer.OnErrorListener l) { mError = l; }
    public void setOnInfoListener(MediaPlayer.OnInfoListener l) { mInfo = l; }
    public void start() { mStart = true; if (mPrepared) mPlayer.start(); }
    public void pause() { mStart = false; if (mPrepared) mPlayer.pause(); }
    public void stopPlayback() { mStart = false; mSeek = 0; close(); }
    public void suspend() { if (mPrepared) mSeek = mPlayer.getCurrentPosition(); close(); }
    public void resume() { open(); }
    public int getDuration() { return mPrepared ? mPlayer.getDuration() : -1; }
    public int getCurrentPosition() { return mPrepared ? mPlayer.getCurrentPosition() : mSeek; }
    public void seekTo(int ms) { if (mPrepared) mPlayer.seekTo(ms); else mSeek = Math.max(0,ms); }
    public boolean isPlaying() { return mPrepared && mPlayer.isPlaying(); }
    public int getBufferPercentage() { return mPrepared ? 100 : 0; }
    public boolean canPause() { return true; }
    public boolean canSeekBackward() { return true; }
    public boolean canSeekForward() { return true; }
    public int getAudioSessionId() { return mPlayer == null ? 0 : mPlayer.getAudioSessionId(); }
    public int resolveAdjustedSize(int desired, int spec) { return getDefaultSize(desired, spec); }
    public void setAudioFocusRequest(int f) {}
    public void setAudioAttributes(android.media.AudioAttributes a) { if (mPlayer != null) mPlayer.setAudioAttributes(a); }
    public void addSubtitleSource(java.io.InputStream in, android.media.MediaFormat f) {}
    public android.os.Looper getSubtitleLooper() { return null; }
}
