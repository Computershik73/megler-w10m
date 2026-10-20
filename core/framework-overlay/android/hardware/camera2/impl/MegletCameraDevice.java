// Meglet: a class of this framework's own, new (the original has no camera2). An open camera as android.hardware.camera2
// gives it to an app -- a CameraDevice, its capture sessions and their requests -- over meglet.CameraNative (core/src/drivers/
// mg_java_camera.c): what CameraX, and so most apps that scan a code today, ask of a camera.
package android.hardware.camera2.impl;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.InputConfiguration;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.media.ImageReader;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * One stream at a time, as a LEGACY camera has it: the camera streams at one size, and each picture goes to every surface of
 * the request it answers -- an ImageReader's as an image (NV21's bytes, given as YUV_420_888's three planes), a SurfaceView's
 * or a SurfaceTexture's as a picture turned upright, mirrored for a front camera, as the phone's compositor turns a camera's
 * buffers on Android. The size is an ImageReader's of the session when there is one (what an app analyses is what it asked
 * for, with no scaling), else the camera's nearest to 1280 x 720.
 *
 * A thread of the camera's own ("Camera2 N") waits for its pictures; each answers the next request queued by capture, or else
 * the repeating one, and its callbacks go to the request's executor in order: onCaptureStarted, then onCaptureCompleted with a
 * TotalCaptureResult whose SENSOR_TIMESTAMP is the image's timestamp (CameraX pairs an image with its result by it), and for
 * the last of a capture's requests onCaptureSequenceCompleted. Focus follows CONTROL_AF_MODE and CONTROL_AF_TRIGGER and is
 * said in CONTROL_AF_STATE; exposure and white balance are the camera's, always CONVERGED; FLASH_MODE TORCH lights the torch.
 *
 * Not here: reprocessing, constrained high-speed and custom sessions (refused, onConfigureFailed), JPEG pictures, manual
 * sensor control, regions, zoom (the crop region is said back, the picture is the whole).
 */
public final class MegletCameraDevice extends CameraDevice {
    static final String TAG = "MegletCamera2";

    private final String mId;
    private final int mIndex;
    private final int[] mInfo, mCaps;
    private final Size[] mSizes;
    private final StateCallback mCallback;
    private final Executor mExecutor;
    private final Object mLock = new Object();
    private long mHandle;
    private boolean mClosed;
    private Session mSession;
    private Thread mThread;
    private Size mStreaming;            // the size the camera streams at, null while it does not
    private long mFrameNumber;
    private int mNextSequence;
    private boolean mTorch;
    // Focus, as CONTROL_AF_STATE says it.
    private int mAfState = CameraMetadata.CONTROL_AF_STATE_INACTIVE;
    private int mAfMode = -1;
    private boolean mAfScanning;
    // The preview's two bitmaps, taking turns, as the old Camera's.
    private final Bitmap[] mShown = new Bitmap[2];
    private int mNext;
    private final Paint mPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private boolean mSaidSize;

    private MegletCameraDevice(int index, String id, StateCallback callback, Executor executor) {
        mIndex = index;
        mId = id;
        mCallback = callback;
        mExecutor = executor;
        mInfo = MegletCameras.info(index);
        mCaps = MegletCameras.caps(index);
        mSizes = MegletCameras.sizes(mCaps);
    }

    /** An executor for a handler, or the calling thread's looper, or the main one, as Android takes a null handler. */
    public static Executor executorOf(Handler handler) {
        Handler h = handler != null ? handler : new Handler(Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper());
        return h::post;
    }

    /** Opens a camera on a thread of its own (it takes a while), then says onOpened, or onError with the device all the same. */
    public static void open(int index, String id, StateCallback callback, Executor executor) {
        if (callback == null) throw new IllegalArgumentException("callback was null");
        MegletCameraDevice d = new MegletCameraDevice(index, id, callback, executor);
        Thread t = new Thread(() -> {
            long h = meglet.CameraNative.open(index);
            synchronized (d.mLock) {
                d.mHandle = h;
                if (h == 0) d.mClosed = true;
            }
            if (h == 0) {
                Log.e(TAG, "camera " + id + " could not be opened");
                executor.execute(() -> callback.onError(d, StateCallback.ERROR_CAMERA_DISABLED));
            } else {
                Log.i(TAG, "camera " + id + " open");
                executor.execute(() -> callback.onOpened(d));
            }
        }, "Camera2 open " + id);
        t.setDaemon(true);
        t.start();
    }

    @Override public String getId() { return mId; }

    @Override public CaptureRequest.Builder createCaptureRequest(int template) {
        synchronized (mLock) {
            if (mClosed) throw new IllegalStateException("CameraDevice was already closed");
        }
        CaptureRequest.Builder b = new CaptureRequest.Builder(null, false, 0, null, null);
        boolean focus = MegletCameras.focuses(mInfo, mCaps);
        Range<Integer>[] fps = MegletCameras.fpsRanges(mCaps);
        Size largest = mSizes[0];
        b.set(CaptureRequest.CONTROL_CAPTURE_INTENT, template == TEMPLATE_STILL_CAPTURE ? CameraMetadata.CONTROL_CAPTURE_INTENT_STILL_CAPTURE
                                                     : template == TEMPLATE_RECORD ? CameraMetadata.CONTROL_CAPTURE_INTENT_VIDEO_RECORD
                                                     : CameraMetadata.CONTROL_CAPTURE_INTENT_PREVIEW);
        b.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
        b.set(CaptureRequest.CONTROL_AF_MODE, !focus ? CameraMetadata.CONTROL_AF_MODE_OFF
                                              : template == TEMPLATE_RECORD ? CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                                              : CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
        b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE);
        b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON);
        b.set(CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER, CameraMetadata.CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps[0]);
        b.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO);
        b.set(CaptureRequest.FLASH_MODE, CameraMetadata.FLASH_MODE_OFF);
        b.set(CaptureRequest.SCALER_CROP_REGION, new Rect(0, 0, largest.getWidth(), largest.getHeight()));
        b.set(CaptureRequest.JPEG_QUALITY, (byte) 95);
        return b;
    }

    @Override public CaptureRequest.Builder createCaptureRequest(int template, Set physicalCameraIds) { return createCaptureRequest(template); }

    @Override public CaptureRequest.Builder createReprocessCaptureRequest(TotalCaptureResult result) {
        throw new IllegalArgumentException("this camera does not reprocess");
    }

    @Override public void createCaptureSession(SessionConfiguration config) {
        if (config == null) throw new IllegalArgumentException("Invalid session configuration");
        List<Surface> surfaces = new ArrayList<>();
        List<Size> deferred = new ArrayList<>();
        for (Object o : config.getOutputConfigurations()) {
            OutputConfiguration oc = (OutputConfiguration) o;
            for (Object s : oc.getSurfaces()) if (s != null) surfaces.add((Surface) s);
            if (oc.getSurfaces().isEmpty() && oc.getConfiguredSize() != null) deferred.add(oc.getConfiguredSize());
        }
        Executor e = config.getExecutor() != null ? config.getExecutor() : executorOf(null);
        session(surfaces, deferred, config.getStateCallback(), e, config.getSessionType() == SessionConfiguration.SESSION_REGULAR);
    }

    @Override public void createCaptureSession(List outputs, CameraCaptureSession.StateCallback callback, Handler handler) {
        List<Surface> surfaces = new ArrayList<>();
        if (outputs != null) for (Object s : outputs) if (s != null) surfaces.add((Surface) s);
        session(surfaces, new ArrayList<>(), callback, executorOf(handler), true);
    }

    @Override public void createCaptureSessionByOutputConfigurations(List outputs, CameraCaptureSession.StateCallback callback, Handler handler) {
        createCaptureSession(new SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outputs, executorOf(handler), callback));
    }

    @Override public void createConstrainedHighSpeedCaptureSession(List outputs, CameraCaptureSession.StateCallback callback, Handler handler) {
        session(new ArrayList<>(), new ArrayList<>(), callback, executorOf(handler), false);
    }

    @Override public void createCustomCaptureSession(InputConfiguration input, List outputs, int mode, CameraCaptureSession.StateCallback callback, Handler handler) {
        session(new ArrayList<>(), new ArrayList<>(), callback, executorOf(handler), false);
    }

    @Override public void createReprocessableCaptureSession(InputConfiguration input, List outputs, CameraCaptureSession.StateCallback callback, Handler handler) {
        session(new ArrayList<>(), new ArrayList<>(), callback, executorOf(handler), false);
    }

    @Override public void createReprocessableCaptureSessionByConfigurations(InputConfiguration input, List outputs, CameraCaptureSession.StateCallback callback, Handler handler) {
        session(new ArrayList<>(), new ArrayList<>(), callback, executorOf(handler), false);
    }

    @Override public boolean isSessionConfigurationSupported(SessionConfiguration config) {
        return config != null && config.getSessionType() == SessionConfiguration.SESSION_REGULAR && config.getInputConfiguration() == null;
    }

    /** A new session, the last one closed (onClosed), and onConfigured then onReady; or onConfigureFailed when `ok` is not. */
    private void session(List<Surface> surfaces, List<Size> deferred, CameraCaptureSession.StateCallback callback, Executor executor, boolean ok) {
        Session last, s;
        synchronized (mLock) {
            if (mClosed) throw new IllegalStateException("CameraDevice was already closed");
            last = mSession;
            s = new Session(surfaces, deferred, callback, executor);
            if (ok) mSession = s;
            else mSession = null;
        }
        if (last != null) last.closeQuietly();
        if (callback == null) return;
        if (!ok) {
            Log.w(TAG, "camera " + mId + ": a session of a kind this camera does not make (reprocessing, high speed, custom)");
            executor.execute(() -> callback.onConfigureFailed(s));
            return;
        }
        Log.i(TAG, "camera " + mId + ": a session of " + surfaces.size() + " surfaces" + (deferred.isEmpty() ? "" : " and " + deferred.size() + " deferred"));
        executor.execute(() -> {
            callback.onConfigured(s);
            callback.onReady(s);
        });
    }

    @Override public void close() {
        Session s;
        Thread t;
        long h;
        synchronized (mLock) {
            if (mClosed && mHandle == 0) return;
            mClosed = true;
            s = mSession;
            mSession = null;
            t = mThread;
            mThread = null;
            h = mHandle;
            mHandle = 0;
            mLock.notifyAll();
        }
        if (s != null) s.closeQuietly();
        if (t != null && t != Thread.currentThread()) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        if (h != 0) {
            if (mTorch) meglet.CameraNative.torch(h, false);
            meglet.CameraNative.stop(h);
            meglet.CameraNative.close(h);
            Log.i(TAG, "camera " + mId + " closed");
            mExecutor.execute(() -> mCallback.onClosed(this));
        }
    }

    // ---------------------------------------------------------------- the stream

    /** The size to stream at: a session's ImageReader's (the largest, if more than one), else the camera's nearest to 1280 x 720. */
    private Size wantedSize(Session s) {
        Size want = null;
        for (Surface o : s.outputs) {
            ImageReader r = o.megletImageReader;
            if (r == null) continue;
            if (want == null || (long) r.getWidth() * r.getHeight() > (long) want.getWidth() * want.getHeight()) want = new Size(r.getWidth(), r.getHeight());
        }
        if (want != null) return want;
        Size best = mSizes[0];
        for (Size z : mSizes) if (Math.abs(z.getWidth() * z.getHeight() - 1280 * 720) < Math.abs(best.getWidth() * best.getHeight() - 1280 * 720)) best = z;
        return best;
    }

    /** Streams at the session's size from now on, the thread started when it is not running; false when the camera refuses. */
    private boolean stream(Session s, CaptureRequest request) {
        Size want = wantedSize(s);
        int fps = 30;
        Object range = request.get(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE);
        if (range instanceof Range && ((Range) range).getUpper() instanceof Integer) fps = (Integer) ((Range) range).getUpper();
        long h;
        synchronized (mLock) {
            if (mClosed || mHandle == 0) return false;
            if (want.equals(mStreaming)) return true;
            h = mHandle;
        }
        if (mStreaming != null) meglet.CameraNative.stop(h);
        boolean ok = meglet.CameraNative.start(h, want.getWidth(), want.getHeight(), fps);
        if (!ok) {
            // A size the camera has not, asked of an ImageReader: the nearest it has (the reader then gets nothing; said once).
            Size near = mSizes[0];
            for (Size z : mSizes)
                if (Math.abs(z.getWidth() - want.getWidth()) + Math.abs(z.getHeight() - want.getHeight()) < Math.abs(near.getWidth() - want.getWidth()) + Math.abs(near.getHeight() - want.getHeight()))
                    near = z;
            Log.w(TAG, "camera " + mId + " does not stream at " + want + "; it streams at " + near + ", and a surface of " + want + " gets no pictures");
            ok = meglet.CameraNative.start(h, near.getWidth(), near.getHeight(), fps);
            want = near;
        }
        synchronized (mLock) {
            mStreaming = ok ? want : null;
            if (ok && mThread == null) {
                mThread = new Thread(this::run, "Camera2 " + mId);
                mThread.setDaemon(true);
                mThread.start();
            }
        }
        if (ok) Log.i(TAG, "camera " + mId + " streams at " + want + ", " + fps + " frames a second at most");
        return ok;
    }

    private void run() {
        while (true) {
            long h;
            synchronized (mLock) {
                if (mClosed || mThread != Thread.currentThread()) break;
                h = mHandle;
            }
            int news = meglet.CameraNative.waitNews(h, 100);
            if ((news & meglet.CameraNative.NEWS_FOCUSED) != 0) focused(true);
            else if ((news & meglet.CameraNative.NEWS_NOT_FOCUSED) != 0) focused(false);
            if ((news & meglet.CameraNative.NEWS_FRAME) != 0) {
                try {
                    picture(h);
                } catch (RuntimeException e) {
                    Log.e(TAG, "camera " + mId + ": a picture could not be given out", e);
                }
            }
            if ((news & meglet.CameraNative.NEWS_GONE) != 0) {
                Log.e(TAG, "camera " + mId + " stopped: it was taken away or is gone");
                mExecutor.execute(() -> mCallback.onDisconnected(this));
                break;
            }
        }
    }

    private void focused(boolean inFocus) {
        synchronized (mLock) {
            if (mAfScanning) {
                mAfScanning = false;
                mAfState = inFocus ? CameraMetadata.CONTROL_AF_STATE_FOCUSED_LOCKED : CameraMetadata.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
            } else if (mAfMode == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE || mAfMode == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO) {
                mAfState = inFocus ? CameraMetadata.CONTROL_AF_STATE_PASSIVE_FOCUSED : CameraMetadata.CONTROL_AF_STATE_PASSIVE_UNFOCUSED;
            }
        }
    }

    /** A request's focus, torch and triggers given to the camera before its picture. */
    private void controls(long h, CaptureRequest r) {
        boolean focus = MegletCameras.focuses(mInfo, mCaps), torchHere = MegletCameras.hasTorch(mInfo, mCaps);
        Object mode = r.get(CaptureRequest.CONTROL_AF_MODE), trigger = r.get(CaptureRequest.CONTROL_AF_TRIGGER), flash = r.get(CaptureRequest.FLASH_MODE);
        int afMode = mode instanceof Integer ? (Integer) mode : CameraMetadata.CONTROL_AF_MODE_OFF;
        if (focus && afMode != mAfMode) {
            mAfMode = afMode;
            boolean continuous = afMode == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE || afMode == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO;
            meglet.CameraNative.focus(h, continuous ? meglet.CameraNative.FOCUS_CONTINUOUS : meglet.CameraNative.FOCUS_NONE);
            synchronized (mLock) {
                mAfState = continuous ? CameraMetadata.CONTROL_AF_STATE_PASSIVE_SCAN : CameraMetadata.CONTROL_AF_STATE_INACTIVE;
            }
        }
        if (focus && trigger instanceof Integer) {
            if ((Integer) trigger == CameraMetadata.CONTROL_AF_TRIGGER_START) {
                synchronized (mLock) {
                    mAfScanning = true;
                    mAfState = CameraMetadata.CONTROL_AF_STATE_ACTIVE_SCAN;
                }
                meglet.CameraNative.focus(h, meglet.CameraNative.FOCUS_ONCE);
            } else if ((Integer) trigger == CameraMetadata.CONTROL_AF_TRIGGER_CANCEL) {
                synchronized (mLock) {
                    mAfScanning = false;
                    mAfState = CameraMetadata.CONTROL_AF_STATE_INACTIVE;
                }
            }
        }
        boolean torch = torchHere && flash instanceof Integer && (Integer) flash == CameraMetadata.FLASH_MODE_TORCH;
        if (torch != mTorch) {
            mTorch = torch;
            meglet.CameraNative.torch(h, torch);
        }
    }

    /** A picture: the next queued request's, else the repeating one's, given to its surfaces, then its callbacks. */
    private void picture(long h) {
        Session s;
        Pending p;
        synchronized (mLock) {
            s = mSession;
            if (s == null || s.closed) return;
            p = s.queued.poll();
            if (p == null) p = s.repeating;
            if (p == null) return;
        }
        final long frame = ++mFrameNumber, timestamp = System.nanoTime();
        final Pending q = p;
        final Session ss = s;
        controls(h, p.request);
        if (p.callback != null) p.executor.execute(() -> q.callback.onCaptureStarted(ss, q.request, timestamp, frame));
        int size = meglet.CameraNative.frameSize(h), fw = size >>> 16, fh = size & 0xffff;
        for (Surface target : p.request.megletTargets()) {
            try {
                give(h, target, fw, fh, timestamp);
            } catch (RuntimeException e) {
                Log.e(TAG, "camera " + mId + ": a picture could not be given to a surface", e);
            }
        }
        final TotalCaptureResult result = result(p, frame, timestamp);
        if (p.callback != null) {
            p.executor.execute(() -> q.callback.onCaptureCompleted(ss, q.request, result));
            if (p.last) p.executor.execute(() -> q.callback.onCaptureSequenceCompleted(ss, q.sequence, frame));
        }
        if (p.last && p != s.repeating) {
            boolean empty;
            synchronized (mLock) {
                empty = s.queued.isEmpty() && s.repeating == null;
            }
            if (empty && s.callback != null) s.executor.execute(() -> ss.callback.onReady(ss));
        }
    }

    /** One picture to one surface: an ImageReader's as NV21's bytes; any other as a picture turned upright. */
    private void give(long h, Surface target, int fw, int fh, long timestamp) {
        ImageReader reader = target.megletImageReader;
        if (reader != null) {
            if (reader.getWidth() != fw || reader.getHeight() != fh) {
                if (!mSaidSize) Log.w(TAG, "camera " + mId + " streams at " + fw + "x" + fh + "; an ImageReader of " + reader.getWidth() + "x" + reader.getHeight() + " gets no pictures");
                mSaidSize = true;
                return;
            }
            byte[] data = reader.megletDequeue();
            if (data == null) return;
            if (meglet.CameraNative.copyFrame(h, data, ImageFormat.NV21) < 0) {
                reader.megletCancel(data);
                return;
            }
            reader.megletQueue(data, timestamp);
            return;
        }
        android.view.SurfaceView view = target.megletView;
        SurfaceTexture texture = target.megletTexture;
        // A SurfaceView whose window has not yet shown the last picture is not given another: the window would draw only the latest.
        if (view != null && !view.megletFrameDrawn()) return;
        boolean front = mInfo[0] == 1;
        int orientation = ((mInfo[1] % 360) + 360) % 360, rot = front ? (360 - orientation) % 360 : orientation;
        if (fw <= 0 || fh <= 0) return;
        int bw = rot % 180 == 0 ? fw : fh, bh = rot % 180 == 0 ? fh : fw;
        Bitmap b = mShown[mNext];
        if (b == null || b.getWidth() != bw || b.getHeight() != bh) b = mShown[mNext] = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888);
        if (!meglet.CameraNative.render(h, b, rot, front)) return;
        boolean posted = false;
        if (view != null) {
            posted = view.megletPostBitmap(b);
        } else if (texture != null) {
            posted = texture.megletPostBitmap(b);
        } else if (target.isValid()) {
            Canvas c = target.lockCanvas(null);
            if (c != null) {
                try {
                    c.drawBitmap(b, null, new Rect(0, 0, c.getWidth(), c.getHeight()), mPaint);
                } finally {
                    target.unlockCanvasAndPost(c);
                }
            }
        }
        if (posted) mNext ^= 1;
    }

    /** The result of a picture: the request's values said back, and the picture's own. */
    private TotalCaptureResult result(Pending p, long frame, long timestamp) {
        TotalCaptureResult r = new TotalCaptureResult((CameraMetadataNative) null, 0);
        for (Object k : p.request.getKeys()) {
            CaptureRequest.Key key = (CaptureRequest.Key) k;
            r.megletSet(new CaptureResult.Key(key.getName(), Object.class), p.request.get(key));
        }
        int afState;
        synchronized (mLock) {
            afState = MegletCameras.focuses(mInfo, mCaps) ? mAfState : CameraMetadata.CONTROL_AF_STATE_INACTIVE;
        }
        r.megletSet(CaptureResult.SENSOR_TIMESTAMP, timestamp);
        r.megletSet(CaptureResult.CONTROL_AF_STATE, afState);
        r.megletSet(CaptureResult.CONTROL_AE_STATE, CameraMetadata.CONTROL_AE_STATE_CONVERGED);
        r.megletSet(CaptureResult.CONTROL_AWB_STATE, CameraMetadata.CONTROL_AWB_STATE_CONVERGED);
        r.megletSet(CaptureResult.FLASH_STATE, MegletCameras.hasTorch(mInfo, mCaps) ? (mTorch ? CameraMetadata.FLASH_STATE_FIRED : CameraMetadata.FLASH_STATE_READY)
                                                                               : CameraMetadata.FLASH_STATE_UNAVAILABLE);
        r.megletSet(CaptureResult.LENS_STATE, CameraMetadata.LENS_STATE_STATIONARY);
        r.megletSet(CaptureResult.SENSOR_FRAME_DURATION, 33333333L);
        r.megletSet(CaptureResult.SENSOR_EXPOSURE_TIME, 16666666L);
        r.megletSet(CaptureResult.SENSOR_SENSITIVITY, 100);
        r.megletSet(CaptureResult.REQUEST_PIPELINE_DEPTH, (byte) 3);
        r.megletSet(CaptureResult.STATISTICS_FACE_DETECT_MODE, CameraMetadata.STATISTICS_FACE_DETECT_MODE_OFF);
        r.megletFrom(mId, p.request, frame, p.sequence);
        return r;
    }

    // ---------------------------------------------------------------- the session

    /** A request waiting for its picture: its callback and executor, its sequence, and whether it ends the sequence. */
    private static final class Pending {
        final CaptureRequest request;
        final CameraCaptureSession.CaptureCallback callback;
        final Executor executor;
        final int sequence;
        final boolean last;

        Pending(CaptureRequest request, CameraCaptureSession.CaptureCallback callback, Executor executor, int sequence, boolean last) {
            this.request = request;
            this.callback = callback;
            this.executor = executor;
            this.sequence = sequence;
            this.last = last;
        }
    }

    final class Session extends CameraCaptureSession {
        final List<Surface> outputs;
        final List<Size> deferred;
        final CameraCaptureSession.StateCallback callback;
        final Executor executor;
        final ArrayDeque<Pending> queued = new ArrayDeque<>();
        Pending repeating;
        boolean closed;

        Session(List<Surface> outputs, List<Size> deferred, CameraCaptureSession.StateCallback callback, Executor executor) {
            this.outputs = outputs;
            this.deferred = deferred;
            this.callback = callback;
            this.executor = executor;
        }

        private void check(CaptureRequest request) {
            if (request == null) throw new IllegalArgumentException("request must not be null");
            synchronized (mLock) {
                if (closed || mClosed) throw new IllegalStateException("Session has been closed; further changes are illegal.");
            }
        }

        private void active(CaptureRequest request) {
            if (stream(this, request) && callback != null) executor.execute(() -> callback.onActive(this));
        }

        private int queue(List requests, CaptureCallback cb, Executor e) {
            if (requests == null || requests.isEmpty()) throw new IllegalArgumentException("Requests must have at least one element");
            for (Object r : requests) check((CaptureRequest) r);
            int sequence;
            synchronized (mLock) {
                sequence = mNextSequence++;
                for (int i = 0; i < requests.size(); i++) queued.add(new Pending((CaptureRequest) requests.get(i), cb, e, sequence, i == requests.size() - 1));
            }
            active((CaptureRequest) requests.get(0));
            return sequence;
        }

        private int repeat(List requests, CaptureCallback cb, Executor e) {
            if (requests == null || requests.isEmpty()) throw new IllegalArgumentException("Requests must have at least one element");
            CaptureRequest r = (CaptureRequest) requests.get(0);
            check(r);
            Pending last;
            int sequence;
            synchronized (mLock) {
                sequence = mNextSequence++;
                last = repeating;
                repeating = new Pending(r, cb, e, sequence, false);
            }
            ended(last);
            active(r);
            return sequence;
        }

        /** A repeating request replaced or stopped: its sequence ends at the last picture it had. */
        private void ended(Pending last) {
            if (last == null || last.callback == null) return;
            final long frame = mFrameNumber;
            last.executor.execute(() -> {
                if (frame > 0) last.callback.onCaptureSequenceCompleted(this, last.sequence, frame);
                else last.callback.onCaptureSequenceAborted(this, last.sequence);
            });
        }

        @Override public int capture(CaptureRequest request, CaptureCallback cb, Handler handler) {
            List<CaptureRequest> l = new ArrayList<>();
            l.add(request);
            return queue(l, cb, executorOf(handler));
        }
        @Override public int captureBurst(List requests, CaptureCallback cb, Handler handler) { return queue(requests, cb, executorOf(handler)); }
        @Override public int captureSingleRequest(CaptureRequest request, Executor e, CaptureCallback cb) {
            List<CaptureRequest> l = new ArrayList<>();
            l.add(request);
            return queue(l, cb, e);
        }
        @Override public int captureBurstRequests(List requests, Executor e, CaptureCallback cb) { return queue(requests, cb, e); }
        @Override public int setRepeatingRequest(CaptureRequest request, CaptureCallback cb, Handler handler) {
            List<CaptureRequest> l = new ArrayList<>();
            l.add(request);
            return repeat(l, cb, executorOf(handler));
        }
        @Override public int setSingleRepeatingRequest(CaptureRequest request, Executor e, CaptureCallback cb) {
            List<CaptureRequest> l = new ArrayList<>();
            l.add(request);
            return repeat(l, cb, e);
        }
        @Override public int setRepeatingBurst(List requests, CaptureCallback cb, Handler handler) { return repeat(requests, cb, executorOf(handler)); }
        @Override public int setRepeatingBurstRequests(List requests, Executor e, CaptureCallback cb) { return repeat(requests, cb, e); }

        @Override public void stopRepeating() {
            Pending last;
            synchronized (mLock) {
                if (closed) throw new IllegalStateException("Session has been closed; further changes are illegal.");
                last = repeating;
                repeating = null;
            }
            ended(last);
            if (callback != null) executor.execute(() -> callback.onReady(this));
        }

        @Override public void abortCaptures() {
            List<Pending> dropped;
            synchronized (mLock) {
                if (closed) throw new IllegalStateException("Session has been closed; further changes are illegal.");
                dropped = new ArrayList<>(queued);
                queued.clear();
            }
            for (Pending p : dropped)
                if (p.last && p.callback != null) p.executor.execute(() -> p.callback.onCaptureSequenceAborted(this, p.sequence));
            if (callback != null) executor.execute(() -> callback.onReady(this));
        }

        @Override public void close() {
            synchronized (mLock) {
                if (mSession == this) mSession = null;
            }
            closeQuietly();
        }

        void closeQuietly() {
            Pending last;
            synchronized (mLock) {
                if (closed) return;
                closed = true;
                last = repeating;
                repeating = null;
                queued.clear();
            }
            ended(last);
            if (callback != null) executor.execute(() -> callback.onClosed(this));
        }

        @Override public CameraDevice getDevice() { return MegletCameraDevice.this; }
        @Override public Surface getInputSurface() { return null; }
        @Override public boolean isReprocessable() { return false; }
        @Override public void prepare(Surface surface) { if (callback != null) executor.execute(() -> callback.onSurfacePrepared(this, surface)); }
        @Override public void prepare(int maxCount, Surface surface) { prepare(surface); }
        @Override public void finalizeOutputConfigurations(List outputs) {
            if (outputs == null) return;
            synchronized (mLock) {
                for (Object o : outputs)
                    for (Object s : ((OutputConfiguration) o).getSurfaces()) if (s != null && !this.outputs.contains(s)) this.outputs.add((Surface) s);
            }
        }
        @Override public void tearDown(Surface surface) {}
    }
}
