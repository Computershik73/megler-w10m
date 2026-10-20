// Overlay of core/framework/gen-src/android/hardware/Camera.java (tools/build-framework.sh), in place of
// the generated stub, which had signatures only: getNumberOfCameras() answered 0, open() a camera that did nothing,
// getParameters() null.
//
// Why. VK mp3 mod signs in only by a QR code now, which it scans with the old camera API (budiyev's code-scanner over ZXing:
// open, the parameters, a preview on a SurfaceView, preview frames in callback buffers, autofocus), and so do most scanners.
//
// What it is: Android 9's class as an app sees it, over the phone's cameras (meglet.CameraNative: core/src/drivers/
// mg_java_camera.c over the shell's meglet_camera_host, Windows.Media.Capture on a phone). Parameters are Android's: a map of
// keys and values, flatten and unflatten with Android's keys, and every getter and setter reading and writing those keys,
// so that code reading get("preview-size-values") works; what a camera's driver ("the HAL") would offer is made here from
// what the shell says the camera does, and setParameters refuses what it does not offer, as a HAL does. CameraInfo, Size,
// Area and the callbacks are Android's; the callbacks run on the looper of the thread that opened the camera, or the main
// looper, as on Android. Preview frames: with setPreviewCallback one at a time (the next is copied only once the last one's
// message is being handled, as Android re-arms its one-shot "barcode scanner" mode), with setPreviewCallbackWithBuffer into
// the app's buffers in the order they were added, a buffer too small taken off the queue and the frame given as null while
// buffers remain (Android's JNI does both). The preview is shown on the SurfaceView (setPreviewDisplay) or the SurfaceTexture
// (setPreviewTexture) as Android shows it: a front camera's picture mirrored, then turned by setDisplayOrientation, then
// scaled to the surface. A thread of the camera's own ("Camera preview N") takes the pictures, shows them and copies them.
//
// Not done: takePicture (refused with a RuntimeException, said in the log), zoom, exposure compensation, focus and metering
// areas (none offered, which Android allows), face detection (refused as Android refuses it where there is none), the
// AutoFocusMoveCallback (never called), RenderScript allocations as preview targets, and a SurfaceTexture drawn by
// OpenGL ES (updateTexImage gives no pixels here: the framework's SurfaceTexture has none for GL; a TextureView shows them).
// android.hardware.camera2 is not this: its list of cameras stays empty (CameraManager, the overlay's), so that an app that
// asks camera2 first and falls back to this API, as CameraX's users cannot, finds this one.
package android.hardware;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;

import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class Camera {
    private static final String TAG = "Camera";

    // The message types of Android's camera service, by which its events reach the EventHandler.
    private static final int CAMERA_MSG_ERROR = 0x001;
    private static final int CAMERA_MSG_SHUTTER = 0x002;
    private static final int CAMERA_MSG_FOCUS = 0x004;
    private static final int CAMERA_MSG_ZOOM = 0x008;
    private static final int CAMERA_MSG_PREVIEW_FRAME = 0x010;
    private static final int CAMERA_MSG_FOCUS_MOVE = 0x800;

    public static final String ACTION_NEW_PICTURE = "android.hardware.action.NEW_PICTURE";
    public static final String ACTION_NEW_VIDEO = "android.hardware.action.NEW_VIDEO";
    public static final int CAMERA_ERROR_UNKNOWN = 1;
    public static final int CAMERA_ERROR_EVICTED = 2;
    public static final int CAMERA_ERROR_DISABLED = 3;
    public static final int CAMERA_ERROR_SERVER_DIED = 100;
    public static final int CAMERA_HAL_API_VERSION_1_0 = 0x100;
    public static final int CAMERA_HAL_API_VERSION_3_0 = 0x300;
    private static final int CAMERA_HAL_API_VERSION_NORMAL_CONNECT = -2;
    private static final int EACCES = 13, ENODEV = 19;

    private volatile long mNative;             // meglet.CameraNative's handle; 0 once released
    private int mCameraId;
    private boolean mFront;
    private int mOrientation;
    private EventHandler mEventHandler;
    private int[] mCaps;
    private Parameters mParams;                // what the camera is set to, as its driver would keep it
    private final Object mLock = new Object(); // the callbacks, the buffers, the preview's targets and state
    private PreviewCallback mPreviewCallback;
    private boolean mOneShot, mWithBuffer, mFrameInFlight;
    private final ArrayList<byte[]> mCallbackBuffers = new ArrayList<>();
    private final Object mAutoFocusCallbackLock = new Object();
    private AutoFocusCallback mAutoFocusCallback;
    private boolean mFocusAsked;
    private AutoFocusMoveCallback mAutoFocusMoveCallback;
    private ErrorCallback mErrorCallback, mDetailedErrorCallback;
    private OnZoomChangeListener mZoomListener;
    private FaceDetectionListener mFaceListener;
    private boolean mFaceDetectionRunning;
    private ShutterCallback mShutterCallback;
    private PictureCallback mRawImageCallback, mPostviewCallback, mJpegCallback;
    private volatile int mDisplayOrientation;
    private SurfaceHolder mHolder;
    private Surface mSurface;
    private SurfaceTexture mTexture;
    private Preview mPreview;
    private boolean mTorch;

    public static int getNumberOfCameras() { return meglet.CameraNative.count(); }
    /** @hide */
    public static int getNumberOfCameras(android.content.Context context) { return getNumberOfCameras(); }

    public static void getCameraInfo(int cameraId, CameraInfo cameraInfo) {
        if (cameraId >= getNumberOfCameras()) throw new RuntimeException("Unknown camera ID");
        int[] v = new int[4];
        if (cameraId < 0 || !meglet.CameraNative.info(cameraId, v)) throw new RuntimeException("Fail to get camera info");
        cameraInfo.facing = v[0];
        cameraInfo.orientation = v[1];
        cameraInfo.canDisableShutterSound = true;
    }
    /** @hide */
    public static void getCameraInfo(int cameraId, android.content.Context context, android.content.res.CameraCompatibilityInfo compat, CameraInfo cameraInfo) {
        getCameraInfo(cameraId, cameraInfo);
    }

    public static Camera open(int cameraId) { return new Camera(cameraId); }
    /** @hide */
    public static Camera open(int cameraId, android.content.Context context, android.content.res.CameraCompatibilityInfo compat) { return new Camera(cameraId); }

    /** The first camera at the back, or null when there is none, as Android's. */
    public static Camera open() {
        int n = getNumberOfCameras();
        CameraInfo info = new CameraInfo();
        for (int i = 0; i < n; i++) {
            getCameraInfo(i, info);
            if (info.facing == CameraInfo.CAMERA_FACING_BACK) return new Camera(i);
        }
        return null;
    }

    /** @hide */
    public static Camera openLegacy(int cameraId, int halVersion) {
        if (halVersion < CAMERA_HAL_API_VERSION_1_0) throw new IllegalArgumentException("Invalid HAL version " + halVersion);
        return new Camera(cameraId);
    }
    /** @hide */
    public static Camera openUninitialized() { return new Camera(); }
    /** @hide */
    public static boolean checkInitErrors(int err) { return err != 0; }
    /** @hide */
    public static boolean shouldExposeAuxCamera() { return false; }

    Camera(int cameraId) {
        int err = cameraInit(cameraId);
        if (checkInitErrors(err)) {
            if (err == -EACCES) throw new RuntimeException("Fail to connect to camera service");
            else if (err == -ENODEV) throw new RuntimeException("Camera initialization failed");
            throw new RuntimeException("Unknown camera error");
        }
    }

    Camera() {}

    /** Android's own order: the event handler on the opener's looper (or the main one), then the connection. */
    private int cameraInit(int cameraId) {
        Looper looper;
        if ((looper = Looper.myLooper()) != null) mEventHandler = new EventHandler(this, looper);
        else if ((looper = Looper.getMainLooper()) != null) mEventHandler = new EventHandler(this, looper);
        else mEventHandler = null;
        // Every way a connection fails is EACCES to the app on Android 9 (its native_setup answers it whenever the service
        // gives no camera): a refused permission, a camera another app has, a number that is no camera. The log says which.
        int[] info = new int[4];
        if (cameraId < 0 || !meglet.CameraNative.info(cameraId, info)) {
            Log.e(TAG, "There is no camera " + cameraId + " (" + getNumberOfCameras() + " cameras)");
            return -EACCES;
        }
        long h = meglet.CameraNative.open(cameraId);
        if (h == 0) return -EACCES;
        mNative = h;
        mCameraId = cameraId;
        mFront = info[0] == CameraInfo.CAMERA_FACING_FRONT;
        mOrientation = info[1];
        mCaps = meglet.CameraNative.caps(h);
        mParams = new Parameters();
        mParams.unflatten(defaults());
        return 0;
    }

    @Override protected void finalize() { release(); }

    private long handle() {
        long h = mNative;
        if (h == 0) throw new RuntimeException("Camera is being used after Camera.release() was called");
        return h;
    }

    public final void release() {
        if (mNative == 0) return;
        stopPreview();
        long h;
        synchronized (mLock) {
            h = mNative;
            mNative = 0;
            mPreviewCallback = null;
            mCallbackBuffers.clear();
            mHolder = null;
            mSurface = null;
            mTexture = null;
        }
        if (h != 0) meglet.CameraNative.close(h);
        mFaceDetectionRunning = false;
    }

    public final void unlock() { handle(); }
    public final void lock() { handle(); }
    public final void reconnect() throws IOException { if (mNative == 0) throw new IOException("reconnect failed"); }

    public final void setPreviewDisplay(SurfaceHolder holder) throws IOException {
        handle();
        synchronized (mLock) {
            mHolder = holder;
            mSurface = holder != null ? holder.getSurface() : null;
            mTexture = null;
        }
    }
    /** @hide */
    public final void setPreviewSurface(Surface surface) throws IOException {
        handle();
        synchronized (mLock) {
            mHolder = null;
            mSurface = surface;
            mTexture = null;
        }
    }
    public final void setPreviewTexture(SurfaceTexture surfaceTexture) throws IOException {
        handle();
        synchronized (mLock) {
            mHolder = null;
            mSurface = null;
            mTexture = surfaceTexture;
        }
    }

    public final void startPreview() {
        long h = handle();
        if (mPreview != null) return;
        Size size = mParams.getPreviewSize();
        int[] fps = new int[2];
        mParams.getPreviewFpsRange(fps);
        int format = mParams.getPreviewFormat();
        // A camera may run anywhere in the range: here at its slow end, but not below 15 frames a second unless the range's top
        // is. A scanner's usual (15000,30000) is then 15 a second, half the pictures to copy into the app's Java and to show.
        int rate = Math.max(fps[0], Math.min(fps[1], 15000));
        if (size == null || !meglet.CameraNative.start(h, size.width, size.height, Math.max(1, (rate + 999) / 1000))) throw new RuntimeException("startPreview failed");
        synchronized (mLock) { mFrameInFlight = false; }
        mPreview = new Preview(h, format);
        mPreview.start();
        // What the parameters say of focusing and the light, which a camera does while it streams.
        applyFocusMode(h);
        mTorch = false;
        applyFlashMode(h);
    }

    public final void stopPreview() {
        Preview p = mPreview;
        mPreview = null;
        if (p != null) {
            p.finish();
            meglet.CameraNative.stop(p.h);
            mTorch = false;
        }
        mFaceDetectionRunning = false;
        mShutterCallback = null;
        mRawImageCallback = null;
        mPostviewCallback = null;
        mJpegCallback = null;
        synchronized (mAutoFocusCallbackLock) {
            mAutoFocusCallback = null;
            mFocusAsked = false;
        }
        mAutoFocusMoveCallback = null;
    }

    /** @hide */
    public final boolean previewEnabled() { return mPreview != null; }

    public final void setPreviewCallback(PreviewCallback cb) {
        synchronized (mLock) {
            mPreviewCallback = cb;
            mOneShot = false;
            mWithBuffer = false;
            mFrameInFlight = false;
            mCallbackBuffers.clear();
        }
    }

    public final void setOneShotPreviewCallback(PreviewCallback cb) {
        synchronized (mLock) {
            mPreviewCallback = cb;
            mOneShot = true;
            mWithBuffer = false;
            mFrameInFlight = false;
            mCallbackBuffers.clear();
        }
    }

    public final void setPreviewCallbackWithBuffer(PreviewCallback cb) {
        synchronized (mLock) {
            mPreviewCallback = cb;
            mOneShot = false;
            mWithBuffer = true;
            mFrameInFlight = false;
            if (cb == null) mCallbackBuffers.clear();
        }
    }

    public final void addCallbackBuffer(byte[] callbackBuffer) {
        if (callbackBuffer == null) {
            Log.e(TAG, "Null byte array!");
            return;
        }
        synchronized (mLock) { mCallbackBuffers.add(callbackBuffer); }
    }

    /** @hide */
    public final void addRawImageCallbackBuffer(byte[] callbackBuffer) {}

    public final void autoFocus(AutoFocusCallback cb) {
        long h = handle();
        synchronized (mAutoFocusCallbackLock) {
            mAutoFocusCallback = cb;
            mFocusAsked = true;
        }
        if (mPreview == null) throw new RuntimeException("autoFocus failed");
        int mode = focusModeOf(mParams.getFocusMode());
        // A camera that does not focus answers at once, in focus, as Android's do.
        if (mode == meglet.CameraNative.FOCUS_NONE || !meglet.CameraNative.focus(h, mode)) focusDone(true);
    }

    public final void cancelAutoFocus() {
        synchronized (mAutoFocusCallbackLock) {
            mAutoFocusCallback = null;
            mFocusAsked = false;
        }
        long h = mNative;
        if (h != 0 && mPreview != null) {
            int mode = focusModeOf(mParams.getFocusMode());
            // Continuous focus goes on (Android: "resumes continuous focus"); a pass of focusing is stopped.
            if (mode != meglet.CameraNative.FOCUS_CONTINUOUS) meglet.CameraNative.focus(h, meglet.CameraNative.FOCUS_NONE);
        }
        if (mEventHandler != null) mEventHandler.removeMessages(CAMERA_MSG_FOCUS);
    }

    public void setAutoFocusMoveCallback(AutoFocusMoveCallback cb) { mAutoFocusMoveCallback = cb; }

    public final void takePicture(ShutterCallback shutter, PictureCallback raw, PictureCallback jpeg) { takePicture(shutter, raw, null, jpeg); }
    public final void takePicture(ShutterCallback shutter, PictureCallback raw, PictureCallback postview, PictureCallback jpeg) {
        handle();
        Log.w(TAG, "takePicture: pictures are not taken here (Meglet has the preview and its frames only)");
        throw new RuntimeException("takePicture failed");
    }

    public final void startSmoothZoom(int value) { handle(); throw new RuntimeException("start smooth zoom failed"); }
    public final void stopSmoothZoom() { handle(); throw new RuntimeException("stop smooth zoom failed"); }

    public final void setDisplayOrientation(int degrees) {
        handle();
        if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) throw new RuntimeException("set display orientation failed");
        mDisplayOrientation = degrees;
    }

    public final boolean enableShutterSound(boolean enabled) { handle(); return true; }
    /** @hide */
    public final boolean disableShutterSound() { return enableShutterSound(false); }

    public final void setZoomChangeListener(OnZoomChangeListener listener) { mZoomListener = listener; }
    public final void setFaceDetectionListener(FaceDetectionListener listener) { mFaceListener = listener; }

    public final void startFaceDetection() {
        if (mFaceDetectionRunning) throw new RuntimeException("Face detection is already running");
        handle();
        throw new IllegalArgumentException("invalid face detection type=0");
    }
    public final void stopFaceDetection() { mFaceDetectionRunning = false; }

    public final void setErrorCallback(ErrorCallback cb) { mErrorCallback = cb; }
    /** @hide */
    public final void setDetailedErrorCallback(ErrorCallback cb) { mDetailedErrorCallback = cb; }

    private int mAudioRestriction;
    public void setAudioRestriction(int mode) { mAudioRestriction = mode; }
    public int getAudioRestriction() { return mAudioRestriction; }

    /** Checked as a camera's driver checks them: what the camera does not offer is refused with "setParameters failed". */
    public void setParameters(Parameters params) {
        long h = handle();
        String bad = refused(params);
        if (bad != null) {
            Log.e(TAG, "setParameters: " + bad);
            throw new RuntimeException("setParameters failed");
        }
        String oldFocus = mParams.getFocusMode(), oldFlash = mParams.getFlashMode();
        mParams.unflatten(params.flatten());
        if (mPreview != null) {
            if (!same(oldFocus, mParams.getFocusMode())) applyFocusMode(h);
            if (!same(oldFlash, mParams.getFlashMode())) applyFlashMode(h);
        }
    }

    public Parameters getParameters() {
        handle();
        Parameters p = new Parameters();
        p.unflatten(mParams.flatten());
        return p;
    }

    /** @hide */
    public static Parameters getEmptyParameters() {
        Camera camera = new Camera();
        return camera.new Parameters();
    }

    /** @hide */
    public static Parameters getParametersCopy(Camera.Parameters parameters) {
        if (parameters == null) throw new NullPointerException("parameters must not be null");
        Camera camera = parameters.getOuter();
        Parameters p = camera.new Parameters();
        p.copyFrom(parameters);
        return p;
    }

    // ---------------------------------------------------------------- what the camera offers

    private static boolean same(String a, String b) { return a == null ? b == null : a.equals(b); }

    private static int focusModeOf(String mode) {
        if (Parameters.FOCUS_MODE_AUTO.equals(mode)) return meglet.CameraNative.FOCUS_ONCE;
        if (Parameters.FOCUS_MODE_MACRO.equals(mode)) return meglet.CameraNative.FOCUS_MACRO;
        if (Parameters.FOCUS_MODE_CONTINUOUS_PICTURE.equals(mode) || Parameters.FOCUS_MODE_CONTINUOUS_VIDEO.equals(mode)) return meglet.CameraNative.FOCUS_CONTINUOUS;
        return meglet.CameraNative.FOCUS_NONE;
    }

    /** Continuous focus starts with its mode; a pass of focusing waits for autoFocus. */
    private void applyFocusMode(long h) {
        int mode = focusModeOf(mParams.getFocusMode());
        meglet.CameraNative.focus(h, mode == meglet.CameraNative.FOCUS_CONTINUOUS ? mode : meglet.CameraNative.FOCUS_NONE);
    }

    private void applyFlashMode(long h) {
        boolean on = Parameters.FLASH_MODE_TORCH.equals(mParams.getFlashMode());
        if (on != mTorch) {
            meglet.CameraNative.torch(h, on);
            mTorch = on;
        }
    }

    private boolean capFocus() { return mCaps != null && mCaps[0] != 0; }
    private boolean capMacro() { return mCaps != null && mCaps[1] != 0; }
    private boolean capTorch() { return mCaps != null && mCaps[2] != 0; }
    private int sizeCount() { return mCaps != null ? mCaps[3] : 0; }
    private int sizeW(int i) { return mCaps[4 + 4 * i]; }
    private int sizeH(int i) { return mCaps[5 + 4 * i]; }
    private int sizeMinFps(int i) { return mCaps[6 + 4 * i]; }
    private int sizeMaxFps(int i) { return mCaps[7 + 4 * i]; }

    /** The parameters a camera's driver would start with, made of what the shell says the camera does. */
    private String defaults() {
        int n = sizeCount();
        StringBuilder sizes = new StringBuilder();
        int preview = -1, video = -1, minFps = Integer.MAX_VALUE, maxFps = 0;
        java.util.TreeSet<Integer> rates = new java.util.TreeSet<>();
        for (int i = 0; i < n; i++) {
            if (sizes.length() > 0) sizes.append(',');
            sizes.append(sizeW(i)).append('x').append(sizeH(i));
            // The preview starts at the largest size within 1280 x 720 (VGA where there is one and nothing larger), as phones'
            // drivers tend to; the video at the largest within 1920 x 1080.
            if (preview < 0 && sizeW(i) <= 1280 && sizeH(i) <= 720) preview = i;
            if (video < 0 && sizeW(i) <= 1920 && sizeH(i) <= 1080) video = i;
            minFps = Math.min(minFps, sizeMinFps(i));
            maxFps = Math.max(maxFps, sizeMaxFps(i));
            rates.add(sizeMinFps(i));
            rates.add(sizeMaxFps(i));
        }
        if (n == 0) {
            sizes.append("640x480");
            minFps = 15;
            maxFps = 30;
            rates.add(15);
            rates.add(30);
        }
        if (preview < 0) preview = n - 1;
        if (video < 0) video = preview;
        String previewSize = n > 0 ? sizeW(preview) + "x" + sizeH(preview) : "640x480";
        String videoSize = n > 0 ? sizeW(video) + "x" + sizeH(video) : "640x480";
        String pictureSize = n > 0 ? sizeW(0) + "x" + sizeH(0) : "640x480";
        StringBuilder rateList = new StringBuilder(), ranges = new StringBuilder();
        for (int r : rates) {
            if (rateList.length() > 0) rateList.append(',');
            rateList.append(r);
        }
        // The ranges: the whole span, and each fixed rate, in Android's order (by the fastest, then the slowest).
        java.util.TreeSet<Long> rangeSet = new java.util.TreeSet<>();
        rangeSet.add(((long) maxFps << 32) | minFps);
        for (int r : rates) rangeSet.add(((long) r << 32) | r);
        for (long r : rangeSet) {
            if (ranges.length() > 0) ranges.append(',');
            ranges.append('(').append((int) r * 1000).append(',').append((int) (r >>> 32) * 1000).append(')');
        }
        StringBuilder p = new StringBuilder();
        p.append("preview-size=").append(previewSize).append(';');
        p.append("preview-size-values=").append(sizes).append(';');
        p.append("preview-format=yuv420sp;preview-format-values=yuv420sp,yuv420p;");
        p.append("preview-frame-rate=").append(maxFps).append(';');
        p.append("preview-frame-rate-values=").append(rateList).append(';');
        p.append("preview-fps-range=").append(minFps * 1000).append(',').append(maxFps * 1000).append(';');
        p.append("preview-fps-range-values=").append(ranges).append(';');
        p.append("picture-size=").append(pictureSize).append(';');
        p.append("picture-size-values=").append(sizes).append(';');
        p.append("picture-format=jpeg;picture-format-values=jpeg;jpeg-quality=95;");
        p.append("jpeg-thumbnail-width=320;jpeg-thumbnail-height=240;jpeg-thumbnail-quality=90;jpeg-thumbnail-size-values=320x240,0x0;");
        if (capFocus()) {
            p.append("focus-mode=auto;focus-mode-values=auto,continuous-picture,continuous-video").append(capMacro() ? ",macro" : "").append(';');
            p.append("focus-distances=0.10,1.20,Infinity;");
        } else {
            p.append("focus-mode=fixed;focus-mode-values=fixed;focus-distances=Infinity,Infinity,Infinity;");
        }
        // (Made up, as nothing here says them: a phone camera's usual focal length and angles of view.)
        p.append("focal-length=4.0;horizontal-view-angle=62.0;vertical-view-angle=48.0;");
        if (capTorch()) p.append("flash-mode=off;flash-mode-values=off,torch;");
        p.append("whitebalance=auto;whitebalance-values=auto;effect=none;effect-values=none;antibanding=auto;antibanding-values=auto;");
        p.append("scene-mode=auto;scene-mode-values=auto;");
        p.append("exposure-compensation=0;max-exposure-compensation=0;min-exposure-compensation=0;exposure-compensation-step=0;");
        p.append("auto-exposure-lock-supported=false;auto-whitebalance-lock-supported=false;");
        p.append("zoom-supported=false;smooth-zoom-supported=false;");
        p.append("max-num-focus-areas=0;max-num-metering-areas=0;max-num-detected-faces-hw=0;max-num-detected-faces-sw=0;");
        p.append("video-size=").append(videoSize).append(';');
        p.append("video-size-values=").append(sizes).append(';');
        p.append("preferred-preview-size-for-video=").append(videoSize).append(';');
        p.append("video-snapshot-supported=false;video-stabilization-supported=false;video-stabilization=false;recording-hint=false");
        return p.toString();
    }

    /** Why a camera's driver would refuse these parameters, or null: a value not among those offered under "<key>-values". */
    private String refused(Parameters params) {
        String[][] lists = {
            { "preview-size", "preview-size-values" }, { "preview-format", "preview-format-values" }, { "preview-frame-rate", "preview-frame-rate-values" },
            { "picture-size", "picture-size-values" }, { "picture-format", "picture-format-values" }, { "focus-mode", "focus-mode-values" },
            { "flash-mode", "flash-mode-values" }, { "whitebalance", "whitebalance-values" }, { "effect", "effect-values" },
            { "antibanding", "antibanding-values" }, { "scene-mode", "scene-mode-values" }, { "video-size", "video-size-values" },
        };
        for (String[] l : lists) {
            String v = params.get(l[0]);
            if (v == null) continue;
            String offered = mParams.get(l[1]);
            if (offered == null) {
                // A camera with no light takes "off" and nothing else for the flash.
                if (l[0].equals("flash-mode") && v.equals("off")) continue;
                return l[0] + "=" + v + ": not offered";
            }
            if (!("," + offered + ",").contains("," + v + ",")) return l[0] + "=" + v + ": not among " + offered;
        }
        String range = params.get("preview-fps-range");
        if (range != null && !mParams.get("preview-fps-range-values").contains("(" + range + ")")) return "preview-fps-range=" + range + ": not among " + mParams.get("preview-fps-range-values");
        String zoom = params.get("zoom");
        if (zoom != null && !zoom.equals("0")) return "zoom=" + zoom + ": there is no zoom";
        String ev = params.get("exposure-compensation");
        if (ev != null && !ev.equals("0")) return "exposure-compensation=" + ev + ": there is none";
        return null;
    }

    // ---------------------------------------------------------------- the preview

    /** A focus that ended: the app's callback hears it if it asked (continuous focus says nothing by itself). */
    private void focusDone(boolean focused) {
        synchronized (mAutoFocusCallbackLock) {
            if (!mFocusAsked) return;
            mFocusAsked = false;
        }
        if (mEventHandler != null) mEventHandler.obtainMessage(CAMERA_MSG_FOCUS, focused ? 1 : 0, 0).sendToTarget();
    }

    /**
     * The camera's thread while it previews: it waits for the camera's news, shows each picture on the preview's surface and
     * hands it to the preview callback. Two bitmaps of the turned size take turns, so that the one the window shows is not the
     * one being filled.
     */
    private final class Preview extends Thread {
        final long h;
        final int format;
        private volatile boolean mRun = true;
        private final Bitmap[] mShown = new Bitmap[2];
        private int mNext;
        private final Paint mPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private int mFrames, mShownCount;

        Preview(long h, int format) {
            super("Camera preview " + mCameraId);
            this.h = h;
            this.format = format;
            setDaemon(true);
        }

        void finish() {
            mRun = false;
            if (Thread.currentThread() == this) return;
            try {
                join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        @Override public void run() {
            while (mRun) {
                int news = meglet.CameraNative.waitNews(h, 100);
                if (!mRun) break;
                if ((news & meglet.CameraNative.NEWS_FOCUSED) != 0) focusDone(true);
                else if ((news & meglet.CameraNative.NEWS_NOT_FOCUSED) != 0) focusDone(false);
                if ((news & meglet.CameraNative.NEWS_FRAME) != 0) {
                    mFrames++;
                    try {
                        show();
                    } catch (RuntimeException e) {
                        Log.e(TAG, "the preview could not be shown", e);
                    }
                    deliver();
                }
                if ((news & meglet.CameraNative.NEWS_GONE) != 0) {
                    Log.e(TAG, "camera " + mCameraId + " stopped: it was taken away or is gone");
                    if (mEventHandler != null) mEventHandler.obtainMessage(CAMERA_MSG_ERROR, CAMERA_ERROR_UNKNOWN, 0).sendToTarget();
                    break;
                }
            }
            Log.d(TAG, "camera " + mCameraId + "'s preview ended: " + mFrames + " pictures, " + mShownCount + " shown");
        }

        private Bitmap bitmap(int w, int ht) {
            Bitmap b = mShown[mNext];
            if (b == null || b.getWidth() != w || b.getHeight() != ht) b = mShown[mNext] = Bitmap.createBitmap(w, ht, Bitmap.Config.ARGB_8888);
            return b;
        }

        /** The picture on the preview's surface, mirrored for a front camera, turned, and scaled there by the view. */
        private void show() {
            SurfaceHolder holder;
            Surface surface;
            SurfaceTexture texture;
            synchronized (mLock) {
                holder = mHolder;
                surface = mSurface;
                texture = mTexture;
            }
            if (holder == null && surface == null && texture == null) return;
            android.view.SurfaceView view = surface != null ? surface.megletView : null;
            // A SurfaceView whose window has not yet shown the last picture is not given another: the window would draw only the latest.
            if (view != null && !view.megletFrameDrawn()) return;
            int size = meglet.CameraNative.frameSize(h), fw = size >>> 16, fh = size & 0xffff, rot = mDisplayOrientation;
            if (fw <= 0 || fh <= 0) return;
            Bitmap b = bitmap(rot % 180 == 0 ? fw : fh, rot % 180 == 0 ? fh : fw);
            if (!meglet.CameraNative.render(h, b, rot, mFront)) return;
            boolean posted;
            if (view != null) {
                posted = view.megletPostBitmap(b);
            } else if (texture != null) {
                posted = texture.megletPostBitmap(b);
            } else {
                // Any other surface: the picture drawn into its canvas, scaled.
                Canvas c = holder != null ? holder.lockCanvas() : surface.lockCanvas(null);
                if (c == null) return;
                try {
                    c.drawBitmap(b, null, new Rect(0, 0, c.getWidth(), c.getHeight()), mPaint);
                } finally {
                    if (holder != null) holder.unlockCanvasAndPost(c);
                    else surface.unlockCanvasAndPost(c);
                }
                posted = false;
            }
            if (posted) mNext ^= 1;
            mShownCount++;
        }

        /** The picture to the preview callback, as Android's JNI copies and posts it. */
        private void deliver() {
            PreviewCallback cb;
            byte[] data = null;
            int size = meglet.CameraNative.frameSize(h);
            int bytes = meglet.CameraNative.frameBytes(size >>> 16, size & 0xffff, format);
            if (bytes <= 0) return;
            synchronized (mLock) {
                cb = mPreviewCallback;
                if (cb == null) return;
                if (mWithBuffer) {
                    if (mCallbackBuffers.isEmpty()) return;
                    byte[] b = mCallbackBuffers.remove(0);
                    if (b.length < bytes) {
                        Log.e(TAG, "Callback buffer was too small! Expected " + bytes + " bytes, but got " + b.length + " bytes!");
                        // Android's: the buffer is gone from the queue, and the frame goes as null while buffers remain.
                        if (mCallbackBuffers.isEmpty()) return;
                    } else {
                        data = b;
                    }
                } else {
                    if (mFrameInFlight) return;
                    mFrameInFlight = true;
                    data = new byte[bytes];
                }
            }
            if (data != null && meglet.CameraNative.copyFrame(h, data, format) < 0) return;
            if (mEventHandler != null) mEventHandler.obtainMessage(CAMERA_MSG_PREVIEW_FRAME, data).sendToTarget();
        }
    }

    private class EventHandler extends Handler {
        private final Camera mCamera;

        EventHandler(Camera c, Looper looper) {
            super(looper);
            mCamera = c;
        }

        @Override public void handleMessage(Message msg) {
            switch (msg.what) {
            case CAMERA_MSG_PREVIEW_FRAME: {
                PreviewCallback pCb;
                synchronized (mLock) {
                    pCb = mPreviewCallback;
                    if (pCb != null) {
                        if (mOneShot) mPreviewCallback = null;
                        else if (!mWithBuffer) mFrameInFlight = false;
                    }
                }
                if (pCb != null) pCb.onPreviewFrame((byte[]) msg.obj, mCamera);
                return;
            }
            case CAMERA_MSG_FOCUS: {
                AutoFocusCallback cb;
                synchronized (mAutoFocusCallbackLock) { cb = mAutoFocusCallback; }
                if (cb != null) cb.onAutoFocus(msg.arg1 != 0, mCamera);
                return;
            }
            case CAMERA_MSG_ERROR: {
                Log.e(TAG, "Error " + msg.arg1);
                if (mDetailedErrorCallback != null) mDetailedErrorCallback.onError(msg.arg1, mCamera);
                else if (mErrorCallback != null) mErrorCallback.onError(msg.arg1, mCamera);
                return;
            }
            default:
                Log.e(TAG, "Unknown message type " + msg.what);
            }
        }
    }

    // ---------------------------------------------------------------- Android's nested classes

    public static class CameraInfo {
        public static final int CAMERA_FACING_BACK = 0;
        public static final int CAMERA_FACING_FRONT = 1;
        public int facing;
        public int orientation;
        public boolean canDisableShutterSound;
        public CameraInfo() {}
    }

    public interface PreviewCallback { void onPreviewFrame(byte[] data, Camera camera); }
    public interface AutoFocusCallback { void onAutoFocus(boolean success, Camera camera); }
    public interface AutoFocusMoveCallback { void onAutoFocusMoving(boolean start, Camera camera); }
    public interface ShutterCallback { void onShutter(); }
    public interface PictureCallback { void onPictureTaken(byte[] data, Camera camera); }
    public interface OnZoomChangeListener { void onZoomChange(int zoomValue, boolean stopped, Camera camera); }
    public interface FaceDetectionListener { void onFaceDetection(Face[] faces, Camera camera); }
    public interface ErrorCallback { void onError(int error, Camera camera); }

    public static class Face {
        public Rect rect;
        public int score;
        public int id = -1;
        public Point leftEye = null;
        public Point rightEye = null;
        public Point mouth = null;
        public Face() {}
    }

    public class Size {
        public int width;
        public int height;

        public Size(int w, int h) {
            width = w;
            height = h;
        }

        @Override public boolean equals(Object obj) {
            if (!(obj instanceof Size)) return false;
            Size s = (Size) obj;
            return width == s.width && height == s.height;
        }

        @Override public int hashCode() { return width * 32713 + height; }
    }

    public static class Area {
        public Rect rect;
        public int weight;

        public Area(Rect rect, int weight) {
            this.rect = rect;
            this.weight = weight;
        }

        @Override public boolean equals(Object obj) {
            if (!(obj instanceof Area)) return false;
            Area a = (Area) obj;
            if (rect == null) {
                if (a.rect != null) return false;
            } else {
                if (!rect.equals(a.rect)) return false;
            }
            return weight == a.weight;
        }
    }

    /** Android's: the camera's settings as keys and values, in the order they were set, flattened as "k=v;k=v". */
    public class Parameters {
        private static final String KEY_PREVIEW_SIZE = "preview-size";
        private static final String KEY_PREVIEW_FORMAT = "preview-format";
        private static final String KEY_PREVIEW_FRAME_RATE = "preview-frame-rate";
        private static final String KEY_PREVIEW_FPS_RANGE = "preview-fps-range";
        private static final String KEY_PICTURE_SIZE = "picture-size";
        private static final String KEY_PICTURE_FORMAT = "picture-format";
        private static final String KEY_JPEG_THUMBNAIL_SIZE = "jpeg-thumbnail-size";
        private static final String KEY_JPEG_THUMBNAIL_WIDTH = "jpeg-thumbnail-width";
        private static final String KEY_JPEG_THUMBNAIL_HEIGHT = "jpeg-thumbnail-height";
        private static final String KEY_JPEG_THUMBNAIL_QUALITY = "jpeg-thumbnail-quality";
        private static final String KEY_JPEG_QUALITY = "jpeg-quality";
        private static final String KEY_ROTATION = "rotation";
        private static final String KEY_GPS_LATITUDE = "gps-latitude";
        private static final String KEY_GPS_LONGITUDE = "gps-longitude";
        private static final String KEY_GPS_ALTITUDE = "gps-altitude";
        private static final String KEY_GPS_TIMESTAMP = "gps-timestamp";
        private static final String KEY_GPS_PROCESSING_METHOD = "gps-processing-method";
        private static final String KEY_WHITE_BALANCE = "whitebalance";
        private static final String KEY_EFFECT = "effect";
        private static final String KEY_ANTIBANDING = "antibanding";
        private static final String KEY_SCENE_MODE = "scene-mode";
        private static final String KEY_FLASH_MODE = "flash-mode";
        private static final String KEY_FOCUS_MODE = "focus-mode";
        private static final String KEY_FOCUS_AREAS = "focus-areas";
        private static final String KEY_MAX_NUM_FOCUS_AREAS = "max-num-focus-areas";
        private static final String KEY_FOCAL_LENGTH = "focal-length";
        private static final String KEY_HORIZONTAL_VIEW_ANGLE = "horizontal-view-angle";
        private static final String KEY_VERTICAL_VIEW_ANGLE = "vertical-view-angle";
        private static final String KEY_EXPOSURE_COMPENSATION = "exposure-compensation";
        private static final String KEY_MAX_EXPOSURE_COMPENSATION = "max-exposure-compensation";
        private static final String KEY_MIN_EXPOSURE_COMPENSATION = "min-exposure-compensation";
        private static final String KEY_EXPOSURE_COMPENSATION_STEP = "exposure-compensation-step";
        private static final String KEY_AUTO_EXPOSURE_LOCK = "auto-exposure-lock";
        private static final String KEY_AUTO_EXPOSURE_LOCK_SUPPORTED = "auto-exposure-lock-supported";
        private static final String KEY_AUTO_WHITEBALANCE_LOCK = "auto-whitebalance-lock";
        private static final String KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED = "auto-whitebalance-lock-supported";
        private static final String KEY_METERING_AREAS = "metering-areas";
        private static final String KEY_MAX_NUM_METERING_AREAS = "max-num-metering-areas";
        private static final String KEY_ZOOM = "zoom";
        private static final String KEY_MAX_ZOOM = "max-zoom";
        private static final String KEY_ZOOM_RATIOS = "zoom-ratios";
        private static final String KEY_ZOOM_SUPPORTED = "zoom-supported";
        private static final String KEY_SMOOTH_ZOOM_SUPPORTED = "smooth-zoom-supported";
        private static final String KEY_FOCUS_DISTANCES = "focus-distances";
        private static final String KEY_VIDEO_SIZE = "video-size";
        private static final String KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO = "preferred-preview-size-for-video";
        private static final String KEY_MAX_NUM_DETECTED_FACES_HW = "max-num-detected-faces-hw";
        private static final String KEY_MAX_NUM_DETECTED_FACES_SW = "max-num-detected-faces-sw";
        private static final String KEY_RECORDING_HINT = "recording-hint";
        private static final String KEY_VIDEO_SNAPSHOT_SUPPORTED = "video-snapshot-supported";
        private static final String KEY_VIDEO_STABILIZATION = "video-stabilization";
        private static final String KEY_VIDEO_STABILIZATION_SUPPORTED = "video-stabilization-supported";
        private static final String SUPPORTED_VALUES_SUFFIX = "-values";
        private static final String TRUE = "true";
        private static final String FALSE = "false";

        public static final String WHITE_BALANCE_AUTO = "auto";
        public static final String WHITE_BALANCE_INCANDESCENT = "incandescent";
        public static final String WHITE_BALANCE_FLUORESCENT = "fluorescent";
        public static final String WHITE_BALANCE_WARM_FLUORESCENT = "warm-fluorescent";
        public static final String WHITE_BALANCE_DAYLIGHT = "daylight";
        public static final String WHITE_BALANCE_CLOUDY_DAYLIGHT = "cloudy-daylight";
        public static final String WHITE_BALANCE_TWILIGHT = "twilight";
        public static final String WHITE_BALANCE_SHADE = "shade";
        public static final String EFFECT_NONE = "none";
        public static final String EFFECT_MONO = "mono";
        public static final String EFFECT_NEGATIVE = "negative";
        public static final String EFFECT_SOLARIZE = "solarize";
        public static final String EFFECT_SEPIA = "sepia";
        public static final String EFFECT_POSTERIZE = "posterize";
        public static final String EFFECT_WHITEBOARD = "whiteboard";
        public static final String EFFECT_BLACKBOARD = "blackboard";
        public static final String EFFECT_AQUA = "aqua";
        public static final String ANTIBANDING_AUTO = "auto";
        public static final String ANTIBANDING_50HZ = "50hz";
        public static final String ANTIBANDING_60HZ = "60hz";
        public static final String ANTIBANDING_OFF = "off";
        public static final String FLASH_MODE_OFF = "off";
        public static final String FLASH_MODE_AUTO = "auto";
        public static final String FLASH_MODE_ON = "on";
        public static final String FLASH_MODE_RED_EYE = "red-eye";
        public static final String FLASH_MODE_TORCH = "torch";
        public static final String SCENE_MODE_AUTO = "auto";
        public static final String SCENE_MODE_ACTION = "action";
        public static final String SCENE_MODE_PORTRAIT = "portrait";
        public static final String SCENE_MODE_LANDSCAPE = "landscape";
        public static final String SCENE_MODE_NIGHT = "night";
        public static final String SCENE_MODE_NIGHT_PORTRAIT = "night-portrait";
        public static final String SCENE_MODE_THEATRE = "theatre";
        public static final String SCENE_MODE_BEACH = "beach";
        public static final String SCENE_MODE_SNOW = "snow";
        public static final String SCENE_MODE_SUNSET = "sunset";
        public static final String SCENE_MODE_STEADYPHOTO = "steadyphoto";
        public static final String SCENE_MODE_FIREWORKS = "fireworks";
        public static final String SCENE_MODE_SPORTS = "sports";
        public static final String SCENE_MODE_PARTY = "party";
        public static final String SCENE_MODE_CANDLELIGHT = "candlelight";
        public static final String SCENE_MODE_BARCODE = "barcode";
        public static final String SCENE_MODE_HDR = "hdr";
        public static final String FOCUS_MODE_AUTO = "auto";
        public static final String FOCUS_MODE_INFINITY = "infinity";
        public static final String FOCUS_MODE_MACRO = "macro";
        public static final String FOCUS_MODE_FIXED = "fixed";
        public static final String FOCUS_MODE_EDOF = "edof";
        public static final String FOCUS_MODE_CONTINUOUS_VIDEO = "continuous-video";
        public static final String FOCUS_MODE_CONTINUOUS_PICTURE = "continuous-picture";
        public static final int FOCUS_DISTANCE_NEAR_INDEX = 0;
        public static final int FOCUS_DISTANCE_OPTIMAL_INDEX = 1;
        public static final int FOCUS_DISTANCE_FAR_INDEX = 2;
        public static final int PREVIEW_FPS_MIN_INDEX = 0;
        public static final int PREVIEW_FPS_MAX_INDEX = 1;

        private static final String PIXEL_FORMAT_YUV422SP = "yuv422sp";
        private static final String PIXEL_FORMAT_YUV420SP = "yuv420sp";
        private static final String PIXEL_FORMAT_YUV422I = "yuv422i-yuyv";
        private static final String PIXEL_FORMAT_YUV420P = "yuv420p";
        private static final String PIXEL_FORMAT_RGB565 = "rgb565";
        private static final String PIXEL_FORMAT_JPEG = "jpeg";
        private static final String PIXEL_FORMAT_BAYER_RGGB = "bayer-rggb";

        private final LinkedHashMap<String, String> mMap;

        private Parameters() { mMap = new LinkedHashMap<String, String>(64); }

        /** @hide */
        public void copyFrom(Parameters other) {
            if (other == null) throw new NullPointerException("other must not be null");
            mMap.putAll(other.mMap);
        }

        private Camera getOuter() { return Camera.this; }

        /** @hide */
        public boolean same(Parameters other) {
            if (this == other) return true;
            return other != null && Parameters.this.mMap.equals(other.mMap);
        }

        @Deprecated public void dump() {
            Log.e(TAG, "dump: size=" + mMap.size());
            for (String k : mMap.keySet()) Log.e(TAG, "dump: " + k + "=" + mMap.get(k));
        }

        public String flatten() {
            StringBuilder flattened = new StringBuilder(128);
            for (String k : mMap.keySet()) {
                flattened.append(k);
                flattened.append("=");
                flattened.append(mMap.get(k));
                flattened.append(";");
            }
            // chop off the extra semicolon at the end
            if (flattened.length() > 0) flattened.deleteCharAt(flattened.length() - 1);
            return flattened.toString();
        }

        public void unflatten(String flattened) {
            mMap.clear();
            for (String kv : splitOn(flattened, ';')) {
                int pos = kv.indexOf('=');
                if (pos == -1) continue;
                mMap.put(kv.substring(0, pos), kv.substring(pos + 1));
            }
        }

        public void remove(String key) { mMap.remove(key); }

        public void set(String key, String value) {
            if (key.indexOf('=') != -1 || key.indexOf(';') != -1 || key.indexOf(0) != -1) {
                Log.e(TAG, "Key \"" + key + "\" contains invalid character (= or ; or \\0)");
                return;
            }
            if (value.indexOf('=') != -1 || value.indexOf(';') != -1 || value.indexOf(0) != -1) {
                Log.e(TAG, "Value \"" + value + "\" contains invalid character (= or ; or \\0)");
                return;
            }
            put(key, value);
        }

        public void set(String key, int value) { put(key, Integer.toString(value)); }

        private void put(String key, String value) {
            // Android takes the key out first so that it goes to the end, as a driver reads them in order.
            mMap.remove(key);
            mMap.put(key, value);
        }

        private void set(String key, List<Area> areas) {
            if (areas == null) {
                set(key, "(0,0,0,0,0)");
            } else {
                StringBuilder buffer = new StringBuilder();
                for (int i = 0; i < areas.size(); i++) {
                    Area area = areas.get(i);
                    Rect rect = area.rect;
                    buffer.append('(').append(rect.left).append(',').append(rect.top).append(',').append(rect.right).append(',').append(rect.bottom).append(',').append(area.weight).append(')');
                    if (i != areas.size() - 1) buffer.append(',');
                }
                set(key, buffer.toString());
            }
        }

        public String get(String key) { return mMap.get(key); }
        public int getInt(String key) { return Integer.parseInt(mMap.get(key)); }

        public void setPreviewSize(int width, int height) { set(KEY_PREVIEW_SIZE, Integer.toString(width) + "x" + Integer.toString(height)); }
        public Size getPreviewSize() { return strToSize(get(KEY_PREVIEW_SIZE)); }
        public List<Size> getSupportedPreviewSizes() { return splitSize(get(KEY_PREVIEW_SIZE + SUPPORTED_VALUES_SUFFIX)); }
        public List<Size> getSupportedVideoSizes() { return splitSize(get(KEY_VIDEO_SIZE + SUPPORTED_VALUES_SUFFIX)); }
        public Size getPreferredPreviewSizeForVideo() { return strToSize(get(KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO)); }

        public void setJpegThumbnailSize(int width, int height) {
            set(KEY_JPEG_THUMBNAIL_WIDTH, width);
            set(KEY_JPEG_THUMBNAIL_HEIGHT, height);
        }
        public Size getJpegThumbnailSize() { return new Size(getInt(KEY_JPEG_THUMBNAIL_WIDTH), getInt(KEY_JPEG_THUMBNAIL_HEIGHT)); }
        public List<Size> getSupportedJpegThumbnailSizes() { return splitSize(get(KEY_JPEG_THUMBNAIL_SIZE + SUPPORTED_VALUES_SUFFIX)); }
        public void setJpegThumbnailQuality(int quality) { set(KEY_JPEG_THUMBNAIL_QUALITY, quality); }
        public int getJpegThumbnailQuality() { return getInt(KEY_JPEG_THUMBNAIL_QUALITY); }
        public void setJpegQuality(int quality) { set(KEY_JPEG_QUALITY, quality); }
        public int getJpegQuality() { return getInt(KEY_JPEG_QUALITY); }

        @Deprecated public void setPreviewFrameRate(int fps) { set(KEY_PREVIEW_FRAME_RATE, fps); }
        @Deprecated public int getPreviewFrameRate() { return getInt(KEY_PREVIEW_FRAME_RATE); }
        @Deprecated public List<Integer> getSupportedPreviewFrameRates() { return splitInt(get(KEY_PREVIEW_FRAME_RATE + SUPPORTED_VALUES_SUFFIX)); }

        public void setPreviewFpsRange(int min, int max) { set(KEY_PREVIEW_FPS_RANGE, "" + min + "," + max); }
        public void getPreviewFpsRange(int[] range) {
            if (range == null || range.length != 2) throw new IllegalArgumentException("range must be an array with two elements.");
            splitInt(get(KEY_PREVIEW_FPS_RANGE), range);
        }
        public List<int[]> getSupportedPreviewFpsRange() { return splitRange(get(KEY_PREVIEW_FPS_RANGE + SUPPORTED_VALUES_SUFFIX)); }

        public void setPreviewFormat(int pixel_format) {
            String s = cameraFormatForPixelFormat(pixel_format);
            if (s == null) throw new IllegalArgumentException("Invalid pixel_format=" + pixel_format);
            set(KEY_PREVIEW_FORMAT, s);
        }
        public int getPreviewFormat() { return pixelFormatForCameraFormat(get(KEY_PREVIEW_FORMAT)); }
        public List<Integer> getSupportedPreviewFormats() {
            String str = get(KEY_PREVIEW_FORMAT + SUPPORTED_VALUES_SUFFIX);
            ArrayList<Integer> formats = new ArrayList<Integer>();
            for (String s : split(str)) {
                int f = pixelFormatForCameraFormat(s);
                if (f == ImageFormat.UNKNOWN) continue;
                formats.add(f);
            }
            return formats;
        }

        public void setPictureSize(int width, int height) { set(KEY_PICTURE_SIZE, Integer.toString(width) + "x" + Integer.toString(height)); }
        public Size getPictureSize() { return strToSize(get(KEY_PICTURE_SIZE)); }
        public List<Size> getSupportedPictureSizes() { return splitSize(get(KEY_PICTURE_SIZE + SUPPORTED_VALUES_SUFFIX)); }
        public void setPictureFormat(int pixel_format) {
            String s = cameraFormatForPixelFormat(pixel_format);
            if (s == null) throw new IllegalArgumentException("Invalid pixel_format=" + pixel_format);
            set(KEY_PICTURE_FORMAT, s);
        }
        public int getPictureFormat() { return pixelFormatForCameraFormat(get(KEY_PICTURE_FORMAT)); }
        public List<Integer> getSupportedPictureFormats() {
            String str = get(KEY_PICTURE_FORMAT + SUPPORTED_VALUES_SUFFIX);
            ArrayList<Integer> formats = new ArrayList<Integer>();
            for (String s : split(str)) {
                int f = pixelFormatForCameraFormat(s);
                if (f == ImageFormat.UNKNOWN) continue;
                formats.add(f);
            }
            return formats;
        }

        private String cameraFormatForPixelFormat(int pixel_format) {
            switch (pixel_format) {
            case ImageFormat.NV16: return PIXEL_FORMAT_YUV422SP;
            case ImageFormat.NV21: return PIXEL_FORMAT_YUV420SP;
            case ImageFormat.YUY2: return PIXEL_FORMAT_YUV422I;
            case ImageFormat.YV12: return PIXEL_FORMAT_YUV420P;
            case ImageFormat.RGB_565: return PIXEL_FORMAT_RGB565;
            case ImageFormat.JPEG: return PIXEL_FORMAT_JPEG;
            default: return null;
            }
        }

        private int pixelFormatForCameraFormat(String format) {
            if (format == null) return ImageFormat.UNKNOWN;
            if (format.equals(PIXEL_FORMAT_YUV422SP)) return ImageFormat.NV16;
            if (format.equals(PIXEL_FORMAT_YUV420SP)) return ImageFormat.NV21;
            if (format.equals(PIXEL_FORMAT_YUV422I)) return ImageFormat.YUY2;
            if (format.equals(PIXEL_FORMAT_YUV420P)) return ImageFormat.YV12;
            if (format.equals(PIXEL_FORMAT_RGB565)) return ImageFormat.RGB_565;
            if (format.equals(PIXEL_FORMAT_JPEG)) return ImageFormat.JPEG;
            return ImageFormat.UNKNOWN;
        }

        public void setRotation(int rotation) {
            if (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) set(KEY_ROTATION, Integer.toString(rotation));
            else throw new IllegalArgumentException("Invalid rotation=" + rotation);
        }
        public void setGpsLatitude(double latitude) { set(KEY_GPS_LATITUDE, Double.toString(latitude)); }
        public void setGpsLongitude(double longitude) { set(KEY_GPS_LONGITUDE, Double.toString(longitude)); }
        public void setGpsAltitude(double altitude) { set(KEY_GPS_ALTITUDE, Double.toString(altitude)); }
        public void setGpsTimestamp(long timestamp) { set(KEY_GPS_TIMESTAMP, Long.toString(timestamp)); }
        public void setGpsProcessingMethod(String processing_method) { set(KEY_GPS_PROCESSING_METHOD, processing_method); }
        public void removeGpsData() {
            remove(KEY_GPS_LATITUDE);
            remove(KEY_GPS_LONGITUDE);
            remove(KEY_GPS_ALTITUDE);
            remove(KEY_GPS_TIMESTAMP);
            remove(KEY_GPS_PROCESSING_METHOD);
        }

        public String getWhiteBalance() { return get(KEY_WHITE_BALANCE); }
        public void setWhiteBalance(String value) {
            String oldValue = get(KEY_WHITE_BALANCE);
            if (sameValue(value, oldValue)) return;
            set(KEY_WHITE_BALANCE, value);
            set(KEY_AUTO_WHITEBALANCE_LOCK, FALSE);
        }
        public List<String> getSupportedWhiteBalance() { return split(get(KEY_WHITE_BALANCE + SUPPORTED_VALUES_SUFFIX)); }
        public String getColorEffect() { return get(KEY_EFFECT); }
        public void setColorEffect(String value) { set(KEY_EFFECT, value); }
        public List<String> getSupportedColorEffects() { return split(get(KEY_EFFECT + SUPPORTED_VALUES_SUFFIX)); }
        public String getAntibanding() { return get(KEY_ANTIBANDING); }
        public void setAntibanding(String antibanding) { set(KEY_ANTIBANDING, antibanding); }
        public List<String> getSupportedAntibanding() { return split(get(KEY_ANTIBANDING + SUPPORTED_VALUES_SUFFIX)); }
        public String getSceneMode() { return get(KEY_SCENE_MODE); }
        public void setSceneMode(String value) { set(KEY_SCENE_MODE, value); }
        public List<String> getSupportedSceneModes() { return split(get(KEY_SCENE_MODE + SUPPORTED_VALUES_SUFFIX)); }
        public String getFlashMode() { return get(KEY_FLASH_MODE); }
        public void setFlashMode(String value) { set(KEY_FLASH_MODE, value); }
        public List<String> getSupportedFlashModes() { return split(get(KEY_FLASH_MODE + SUPPORTED_VALUES_SUFFIX)); }
        public String getFocusMode() { return get(KEY_FOCUS_MODE); }
        public void setFocusMode(String value) { set(KEY_FOCUS_MODE, value); }
        public List<String> getSupportedFocusModes() { return split(get(KEY_FOCUS_MODE + SUPPORTED_VALUES_SUFFIX)); }
        public float getFocalLength() { return Float.parseFloat(get(KEY_FOCAL_LENGTH)); }
        public float getHorizontalViewAngle() { return Float.parseFloat(get(KEY_HORIZONTAL_VIEW_ANGLE)); }
        public float getVerticalViewAngle() { return Float.parseFloat(get(KEY_VERTICAL_VIEW_ANGLE)); }

        public int getExposureCompensation() { return getInt(KEY_EXPOSURE_COMPENSATION, 0); }
        public void setExposureCompensation(int value) { set(KEY_EXPOSURE_COMPENSATION, value); }
        public int getMaxExposureCompensation() { return getInt(KEY_MAX_EXPOSURE_COMPENSATION, 0); }
        public int getMinExposureCompensation() { return getInt(KEY_MIN_EXPOSURE_COMPENSATION, 0); }
        public float getExposureCompensationStep() { return getFloat(KEY_EXPOSURE_COMPENSATION_STEP, 0); }
        public void setAutoExposureLock(boolean toggle) { set(KEY_AUTO_EXPOSURE_LOCK, toggle ? TRUE : FALSE); }
        public boolean getAutoExposureLock() { return TRUE.equals(get(KEY_AUTO_EXPOSURE_LOCK)); }
        public boolean isAutoExposureLockSupported() { return TRUE.equals(get(KEY_AUTO_EXPOSURE_LOCK_SUPPORTED)); }
        public void setAutoWhiteBalanceLock(boolean toggle) { set(KEY_AUTO_WHITEBALANCE_LOCK, toggle ? TRUE : FALSE); }
        public boolean getAutoWhiteBalanceLock() { return TRUE.equals(get(KEY_AUTO_WHITEBALANCE_LOCK)); }
        public boolean isAutoWhiteBalanceLockSupported() { return TRUE.equals(get(KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED)); }

        public int getZoom() { return getInt(KEY_ZOOM, 0); }
        public void setZoom(int value) { set(KEY_ZOOM, value); }
        public boolean isZoomSupported() { return TRUE.equals(get(KEY_ZOOM_SUPPORTED)); }
        public int getMaxZoom() { return getInt(KEY_MAX_ZOOM, 0); }
        public List<Integer> getZoomRatios() { return splitInt(get(KEY_ZOOM_RATIOS)); }
        public boolean isSmoothZoomSupported() { return TRUE.equals(get(KEY_SMOOTH_ZOOM_SUPPORTED)); }

        public void getFocusDistances(float[] output) {
            if (output == null || output.length != 3) throw new IllegalArgumentException("output must be a float array with three elements.");
            splitFloat(get(KEY_FOCUS_DISTANCES), output);
        }

        public int getMaxNumFocusAreas() { return getInt(KEY_MAX_NUM_FOCUS_AREAS, 0); }
        public List<Area> getFocusAreas() { return splitArea(get(KEY_FOCUS_AREAS)); }
        public void setFocusAreas(List<Area> focusAreas) { set(KEY_FOCUS_AREAS, focusAreas); }
        public int getMaxNumMeteringAreas() { return getInt(KEY_MAX_NUM_METERING_AREAS, 0); }
        public List<Area> getMeteringAreas() { return splitArea(get(KEY_METERING_AREAS)); }
        public void setMeteringAreas(List<Area> meteringAreas) { set(KEY_METERING_AREAS, meteringAreas); }
        public int getMaxNumDetectedFaces() { return getInt(KEY_MAX_NUM_DETECTED_FACES_HW, 0); }

        public void setRecordingHint(boolean hint) { set(KEY_RECORDING_HINT, hint ? TRUE : FALSE); }
        public boolean isVideoSnapshotSupported() { return TRUE.equals(get(KEY_VIDEO_SNAPSHOT_SUPPORTED)); }
        public void setVideoStabilization(boolean toggle) { set(KEY_VIDEO_STABILIZATION, toggle ? TRUE : FALSE); }
        public boolean getVideoStabilization() { return TRUE.equals(get(KEY_VIDEO_STABILIZATION)); }
        public boolean isVideoStabilizationSupported() { return TRUE.equals(get(KEY_VIDEO_STABILIZATION_SUPPORTED)); }

        // Android's parsing, value by value.

        private ArrayList<String> splitOn(String str, char c) {
            ArrayList<String> out = new ArrayList<String>();
            if (str == null || str.isEmpty()) return out;
            int from = 0;
            for (;;) {
                int at = str.indexOf(c, from);
                if (at < 0) {
                    out.add(str.substring(from));
                    return out;
                }
                out.add(str.substring(from, at));
                from = at + 1;
            }
        }

        private ArrayList<String> split(String str) {
            if (str == null) return null;
            return splitOn(str, ',');
        }

        private ArrayList<Integer> splitInt(String str) {
            if (str == null) return null;
            ArrayList<Integer> substrings = new ArrayList<Integer>();
            for (String s : splitOn(str, ',')) substrings.add(Integer.parseInt(s));
            if (substrings.size() == 0) return null;
            return substrings;
        }

        private void splitInt(String str, int[] output) {
            if (str == null) return;
            int index = 0;
            for (String s : splitOn(str, ',')) output[index++] = Integer.parseInt(s);
        }

        private void splitFloat(String str, float[] output) {
            if (str == null) return;
            int index = 0;
            for (String s : splitOn(str, ',')) output[index++] = Float.parseFloat(s);
        }

        private float getFloat(String key, float defaultValue) {
            try {
                return Float.parseFloat(mMap.get(key));
            } catch (NumberFormatException | NullPointerException ex) {
                return defaultValue;
            }
        }

        private int getInt(String key, int defaultValue) {
            try {
                return Integer.parseInt(mMap.get(key));
            } catch (NumberFormatException ex) {
                return defaultValue;
            }
        }

        private ArrayList<Size> splitSize(String str) {
            if (str == null) return null;
            ArrayList<Size> sizeList = new ArrayList<Size>();
            for (String s : splitOn(str, ',')) {
                Size size = strToSize(s);
                if (size != null) sizeList.add(size);
            }
            if (sizeList.size() == 0) return null;
            return sizeList;
        }

        private Size strToSize(String str) {
            if (str == null) return null;
            int pos = str.indexOf('x');
            if (pos != -1) return new Size(Integer.parseInt(str.substring(0, pos)), Integer.parseInt(str.substring(pos + 1)));
            Log.e(TAG, "Invalid size parameter string=" + str);
            return null;
        }

        private ArrayList<int[]> splitRange(String str) {
            if (str == null || str.charAt(0) != '(' || str.charAt(str.length() - 1) != ')') {
                Log.e(TAG, "Invalid range list string=" + str);
                return null;
            }
            ArrayList<int[]> rangeList = new ArrayList<int[]>();
            int endIndex, fromIndex = 1;
            do {
                int[] range = new int[2];
                endIndex = str.indexOf("),(", fromIndex);
                if (endIndex == -1) endIndex = str.length() - 1;
                splitInt(str.substring(fromIndex, endIndex), range);
                rangeList.add(range);
                fromIndex = endIndex + 3;
            } while (endIndex != str.length() - 1);
            if (rangeList.size() == 0) return null;
            return rangeList;
        }

        private ArrayList<Area> splitArea(String str) {
            if (str == null || str.charAt(0) != '(' || str.charAt(str.length() - 1) != ')') {
                Log.e(TAG, "Invalid area string=" + str);
                return null;
            }
            ArrayList<Area> result = new ArrayList<Area>();
            int endIndex, fromIndex = 1;
            int[] array = new int[5];
            do {
                endIndex = str.indexOf("),(", fromIndex);
                if (endIndex == -1) endIndex = str.length() - 1;
                splitInt(str.substring(fromIndex, endIndex), array);
                result.add(new Area(new Rect(array[0], array[1], array[2], array[3]), array[4]));
                fromIndex = endIndex + 3;
            } while (endIndex != str.length() - 1);
            if (result.size() == 0) return null;
            if (result.size() == 1) {
                Area area = result.get(0);
                Rect rect = area.rect;
                if (rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0 && area.weight == 0) return null;
            }
            return result;
        }

        private boolean sameValue(String s1, String s2) {
            if (s1 == null && s2 == null) return true;
            if (s1 != null && s1.equals(s2)) return true;
            return false;
        }
    }
}
