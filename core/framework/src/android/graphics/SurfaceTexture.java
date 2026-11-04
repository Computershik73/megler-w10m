package android.graphics;

/**
 * A stream of frames from a producer (a Surface made on it: MediaCodec's video, lockCanvas drawing, an EGL window surface) to a
 * consumer. Megler keeps the frames in software: a back buffer the producer draws, and the front one the consumer (a TextureView)
 * shows. GL consumers (updateTexImage into an external texture) get no pixels yet.
 */
public class SurfaceTexture {
    public interface OnFrameAvailableListener { void onFrameAvailable(SurfaceTexture t); }
    public interface OnSetFrameRateListener { void onSetFrameRate(SurfaceTexture t, float rate, int compat, int strategy); }
    public static class OutOfResourcesException extends Exception { public OutOfResourcesException() {} public OutOfResourcesException(String s) { super(s); } }
    private int mTex;
    private final Object mLock = new Object();
    private int mW, mH;
    private Bitmap mFront, mBack;
    private Canvas mFrontCanvas, mBackCanvas;
    private boolean mLocked, mReleased;
    private long mTimestamp;
    private OnFrameAvailableListener mListener;
    private android.os.Handler mHandler;
    /** The TextureView showing this texture, told when a frame arrives. */
    public android.view.View meglerView;
    public SurfaceTexture(int texName) { this(texName, false); }
    public SurfaceTexture(int texName, boolean singleBufferMode) { mTex = texName; }
    public SurfaceTexture(boolean singleBufferMode) { mTex = 0; }
    public void setOnFrameAvailableListener(OnFrameAvailableListener l) { setOnFrameAvailableListener(l, null); }
    public void setOnFrameAvailableListener(OnFrameAvailableListener l, android.os.Handler h) {
        mListener = l;
        mHandler = l == null ? null : h != null ? h : new android.os.Handler(android.os.Looper.myLooper() != null ? android.os.Looper.myLooper() : android.os.Looper.getMainLooper());
    }
    public void setOnSetFrameRateListener(OnSetFrameRateListener l, android.os.Handler h) {}
    public void setDefaultBufferSize(int w, int h) { synchronized (mLock) { mW = w; mH = h; } }
    /** The buffer size producers draw at (the consumer's size when it never set one). */
    public int[] meglerSize() { synchronized (mLock) { return new int[] { mW, mH }; } }
    public Canvas meglerLock(Rect dirty) {
        synchronized (mLock) {
            if (mReleased) return null;
            while (mLocked) { try { mLock.wait(); } catch (InterruptedException e) { return null; } if (mReleased) return null; }
            int w = mW > 0 ? mW : 1, h = mH > 0 ? mH : 1;
            if (mBack == null || mBack.getWidth() != w || mBack.getHeight() != h) {
                if (mBackCanvas != null) mBackCanvas.meglerRelease();
                mBack = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                mBackCanvas = null;
            }
            // A bitmap supplied directly by a producer has no owned canvas yet.
            if (mBackCanvas == null) mBackCanvas = new Canvas(mBack);
            mLocked = true;
            mBackCanvas.save();
            if (dirty != null) mBackCanvas.clipRect(dirty);
            return mBackCanvas;
        }
    }
    public void meglerPost(Canvas c) {
        synchronized (mLock) {
            if (!mLocked) return;
            if (mReleased) {
                mLocked = false;
                releaseCanvases();
                mLock.notifyAll();
                return;
            }
            mBackCanvas.restore();
            Bitmap f = mFront; mFront = mBack; mBack = f;
            // The canvas belongs to its buffer. Creating another on every post
            // leaves competing GPU copies until GC and forces their pixels back
            // to CPU memory before the next drawing.
            Canvas fc = mFrontCanvas; mFrontCanvas = mBackCanvas; mBackCanvas = fc;
            if (mBack != null && (mBack.getWidth() != mFront.getWidth() || mBack.getHeight() != mFront.getHeight())) {
                if (mBackCanvas != null) mBackCanvas.meglerRelease();
                mBackCanvas = null;
                mBack = null;
            }
            mLocked = false; mTimestamp = System.nanoTime();
            mLock.notifyAll();
        }
        android.view.View v = meglerView;
        if (v != null) v.postInvalidate();
        OnFrameAvailableListener l = mListener; android.os.Handler h = mHandler;
        if (l != null && h != null) h.post(() -> l.onFrameAvailable(this));
    }
    public boolean meglerValid() { return !mReleased; }
    private Bitmap mStage;
    /** A native producer's frame (core/src/drivers/mg_java_ndk.c's TextureView windows, ANativeWindow_lock): w x h RGBA at addr, bottom row first when flip. */
    public void meglerPostRgba(long addr, int w, int h, boolean flip) {
        if (mStage == null || mStage.getWidth() != w || mStage.getHeight() != h) mStage = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        mStage.meglerSetRgba(addr);
        Canvas c = meglerLock(null);
        if (c == null) return;
        try {
            c.save();
            if (flip) c.scale(1, -1, c.getWidth() / 2f, c.getHeight() / 2f);
            c.drawColor(0, PorterDuff.Mode.CLEAR);
            c.drawBitmap(mStage, null, new Rect(0, 0, c.getWidth(), c.getHeight()), null);
            c.restore();
        } finally { meglerPost(c); }
    }
    /** The latest frame (null before the first). */
    public Bitmap meglerFront() { synchronized (mLock) { return mFront; } }
    public void updateTexImage() { if (mReleased) throw new IllegalStateException("updateTexImage on a released SurfaceTexture"); }
    public void releaseTexImage() {}
    public void detachFromGLContext() { mTex = 0; }
    public void attachToGLContext(int tex) { mTex = tex; }
    public void getTransformMatrix(float[] m) { android.opengl.Matrix.setIdentityM(m, 0); }
    public long getTimestamp() { return mTimestamp; }
    public int getDataSpace() { return 0; }
    public boolean isSingleBuffered() { return false; }
    public void release() {
        synchronized (mLock) {
            if (mReleased) return;
            mReleased = true;
            // A producer may still be drawing into the locked canvas. Retire
            // it when posted, rather than freeing its native state underneath it.
            if (!mLocked) releaseCanvases();
            mLock.notifyAll();
        }
    }
    private void releaseCanvases() {
        if (mFrontCanvas != null) mFrontCanvas.meglerRelease();
        if (mBackCanvas != null) mBackCanvas.meglerRelease();
        mFrontCanvas = mBackCanvas = null;
    }
    public boolean isReleased() { return mReleased; }
}
