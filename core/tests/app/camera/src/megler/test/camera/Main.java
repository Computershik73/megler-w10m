package megler.test.camera;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The old camera API as a code scanner uses it (core/src/drivers/hk_java_camera.c and the overlay's android.hardware.Camera),
 * on the test's stand-in for the shell's cameras (core/tests/hk_camera_standin.c): a camera at the back turned 90 degrees,
 * with focus and a torch, and one at the front turned 270, whose pictures are four quarters of known colours with their
 * number in the first four bytes. The top half of the screen is a SurfaceView for the back camera's preview, the bottom
 * half a TextureView for the front camera's. A thread of the app's goes through the steps and says how each went; the back
 * camera is opened on a HandlerThread, whose looper its callbacks must come on, the front one on that plain thread, whose
 * callbacks must come on the main looper. While a preview runs for the test to look at, the app waits for a tap.
 *
 * The TextureView is framed as VK mp3 mod frames its scanner's (BarcodeViewfinderLayout, below): its corners painted over
 * with a path toggled to inverse, and a marker at each corner drawn in a layer.
 *
 * When the test has the stand-in refuse every camera (as Windows does when the user has said no), the first open fails as
 * Android's does, and that is the whole run.
 */
public class Main extends Activity {
    private static final String T = "CameraTest";
    /* The stand-in's quarters, luma, U, V: top left red, top right green, bottom left blue, bottom right white. */
    private static final int[][] QUARTERS = { { 81, 90, 240 }, { 145, 54, 34 }, { 41, 240, 110 }, { 235, 128, 128 } };
    private SurfaceView surfaceView;
    private TextureView textureView;
    private final CountDownLatch surfaceReady = new CountDownLatch(1), textureReady = new CountDownLatch(1);
    private volatile CountDownLatch tapped = new CountDownLatch(1);
    private volatile boolean permissionGranted;
    private Handler cameraHandler;
    private int ok, failed;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) { surfaceReady.countDown(); }
            public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {}
            public void surfaceDestroyed(SurfaceHolder h) {}
        });
        root.addView(surfaceView, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        textureView = new TextureView(this);
        textureView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            public void onSurfaceTextureAvailable(SurfaceTexture t, int w, int h) { textureReady.countDown(); }
            public void onSurfaceTextureSizeChanged(SurfaceTexture t, int w, int h) {}
            public boolean onSurfaceTextureDestroyed(SurfaceTexture t) { return true; }
            public void onSurfaceTextureUpdated(SurfaceTexture t) {}
        });
        Viewfinder frame = new Viewfinder(this);
        frame.addView(textureView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(frame, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        HandlerThread looper = new HandlerThread("CameraLooper");
        looper.start();
        cameraHandler = new Handler(looper.getLooper());
        new Thread(this::steps, "steps").start();
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_UP) tapped.countDown();
        return super.dispatchTouchEvent(e);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) return;
        int[] s = new int[2], t = new int[2];
        surfaceView.getLocationOnScreen(s);
        textureView.getLocationOnScreen(t);
        Log.i(T, "layout surface=" + s[0] + "," + s[1] + "," + surfaceView.getWidth() + "x" + surfaceView.getHeight() + " texture=" + t[0] + "," + t[1] + ","
                + textureView.getWidth() + "x" + textureView.getHeight());
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        permissionGranted = request == 5 && results.length == 1 && results[0] == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }

    /**
     * What VK mp3 mod's BarcodeViewfinderLayout does round its QR scanner's TextureView: once its children have drawn, it rounds
     * the corners by filling a rounded rectangle the size of its canvas, toggled to inverse in its constructor, in white -- the
     * four corners outside it -- and draws a marker at each corner in a layer of the canvas's size, turned about its middle,
     * with a paint that clears under a translucent one. On Android the canvas is the view's own (a hardware-accelerated
     * view draws into a display list of its size).
     */
    private static final class Viewfinder extends FrameLayout {
        private final Path invRoundRect = new Path(), marker = new Path();
        private final Paint white = new Paint(), fill = new Paint(), clear = new Paint(), stroke = new Paint();

        Viewfinder(Context c) {
            super(c);
            setWillNotDraw(false);
            white.setColor(0xFFFFFFFF);
            fill.setColor(0x99FFFFFF);
            clear.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.CLEAR));
            clear.setColor(0xFF000000);
            stroke.setColor(0x26000000);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeWidth(2);
            marker.moveTo(0, 0);
            marker.rLineTo(30, 0);
            marker.rLineTo(0, 8);
            marker.rLineTo(-22, 0);
            marker.rLineTo(0, 22);
            marker.rLineTo(-8, 0);
            marker.close();
            invRoundRect.toggleInverseFillType();
        }

        @Override protected void dispatchDraw(Canvas canvas) {
            super.dispatchDraw(canvas);
            if (invRoundRect.isEmpty()) invRoundRect.addRoundRect(new RectF(0, 0, canvas.getWidth(), canvas.getHeight()), 24, 24, Path.Direction.CW);
            canvas.drawPath(invRoundRect, white);
            canvas.saveLayer(0, 0, canvas.getWidth(), canvas.getHeight(), null, Canvas.ALL_SAVE_FLAG);
            for (int i = 0; i < 4; i++) {
                canvas.save();
                canvas.rotate(90 * i, canvas.getWidth() / 2f, canvas.getHeight() / 2f);
                canvas.translate(16, 16);
                canvas.drawPath(marker, stroke);
                canvas.drawPath(marker, clear);
                canvas.drawPath(marker, fill);
                canvas.restore();
            }
            canvas.restore();
        }
    }

    /* ------------------------------------------------------------ the steps */

    private interface Step { boolean run() throws Exception; }

    private void step(String name, Step s) {
        boolean good;
        try {
            good = s.run();
        } catch (Throwable e) {
            Log.e(T, name + " threw", e);
            good = false;
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED"));
    }

    /** Runs `r` on the camera's looper thread and waits for it. */
    private <V> V onLooper(java.util.concurrent.Callable<V> r) throws Exception {
        final Object[] out = new Object[2];
        final CountDownLatch done = new CountDownLatch(1);
        cameraHandler.post(() -> {
            try {
                out[0] = r.call();
            } catch (Throwable e) {
                out[1] = e;
            }
            done.countDown();
        });
        if (!done.await(30, TimeUnit.SECONDS)) throw new IllegalStateException("the camera's looper did not answer");
        if (out[1] instanceof Exception) throw (Exception) out[1];
        if (out[1] != null) throw new RuntimeException((Throwable) out[1]);
        return (V) out[0];
    }

    private static String message(Throwable e) { return e.getClass().getName() + ": " + e.getMessage(); }

    /** The frame's number (the stand-in's first four bytes), and whether every other byte of its NV21 is its quarter's. */
    private static int number(byte[] d) { return (d[0] & 255) | (d[1] & 255) << 8 | (d[2] & 255) << 16 | (d[3] & 255) << 24; }

    private static boolean right(byte[] d, int w, int h) {
        if (d == null || d.length < w * h * 3 / 2) return false;
        for (int y = 0; y < h; y++)
            for (int x = y == 0 ? 4 : 0; x < w; x++)
                if ((d[y * w + x] & 255) != QUARTERS[(y < h / 2 ? 0 : 2) + (x < w / 2 ? 0 : 1)][0]) return false;
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++) {
                int[] q = QUARTERS[(y < h / 4 ? 0 : 2) + (x < w / 4 ? 0 : 1)];
                int at = w * h + (y * (w / 2) + x) * 2;
                if ((d[at] & 255) != q[2] || (d[at + 1] & 255) != q[1]) return false;
            }
        return true;
    }

    /** What a preview callback was given: the arrays (null kept), their numbers and threads, and whether each was right. */
    private static final class Frames implements Camera.PreviewCallback {
        final int w, h;
        final ArrayList<byte[]> arrays = new ArrayList<>();
        final ArrayList<Integer> numbers = new ArrayList<>();
        final ArrayList<String> threads = new ArrayList<>();
        volatile boolean allRight = true;
        final long sleepMs;
        Frames(int w, int h, long sleepMs) { this.w = w; this.h = h; this.sleepMs = sleepMs; }
        public void onPreviewFrame(byte[] data, Camera camera) {
            synchronized (this) {
                arrays.add(data);
                numbers.add(data != null ? number(data) : -1);
                threads.add(Thread.currentThread().getName());
                if (data != null && !right(data, w, h)) allRight = false;
                notifyAll();
            }
            if (sleepMs > 0) {
                try {
                    Thread.sleep(sleepMs);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }
        synchronized int count() { return arrays.size(); }
        synchronized boolean await(int n, long ms) throws InterruptedException {
            long until = System.currentTimeMillis() + ms;
            while (arrays.size() < n && System.currentTimeMillis() < until) wait(Math.max(1, until - System.currentTimeMillis()));
            return arrays.size() >= n;
        }
        /** Each number above the one before. */
        synchronized boolean rising() {
            int last = 0;
            for (int n : numbers) {
                if (n == -1) continue;
                if (n <= last) return false;
                last = n;
            }
            return true;
        }
        synchronized boolean allOn(String thread) {
            for (String t : threads) if (!t.equals(thread)) return false;
            return true;
        }
    }

    private static final class Focus implements Camera.AutoFocusCallback {
        volatile int calls;
        volatile boolean success;
        volatile String thread;
        final CountDownLatch done = new CountDownLatch(1);
        public void onAutoFocus(boolean s, Camera camera) {
            calls++;
            success = s;
            thread = Thread.currentThread().getName();
            done.countDown();
        }
    }

    private static boolean has(List<String> l, String... want) { return l != null && l.containsAll(Arrays.asList(want)); }

    private static boolean hasSize(List<Camera.Size> l, int w, int h) {
        if (l == null) return false;
        for (Camera.Size s : l) if (s.width == w && s.height == h) return true;
        return false;
    }

    private Camera back;

    private void steps() {
        /* Refused (the test's second run): the first open fails as Android's does, and nothing else is tried. */
        try {
            Camera c = onLooper(() -> Camera.open(0));
            c.release();
        } catch (RuntimeException e) {
            Log.i(T, "open refused " + message(e) + " cameras=" + Camera.getNumberOfCameras());
            step("denied", () -> "Fail to connect to camera service".equals(e.getMessage()) && Camera.getNumberOfCameras() == 2);
            Log.i(T, "done ok=" + ok + " failed=" + failed);
            return;
        } catch (Exception e) {
            Log.e(T, "the first open did not come back", e);
        }

        step("permission", () -> {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, 5);
            for (int i = 0; i < 100 && !permissionGranted; i++) Thread.sleep(50);
            return checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED && permissionGranted;
        });
        step("features", () -> {
            PackageManager pm = getPackageManager();
            return pm.hasSystemFeature(PackageManager.FEATURE_CAMERA) && pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)
                    && pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_FRONT) && pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_AUTOFOCUS)
                    && pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_FLASH) && !pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_EXTERNAL);
        });
        step("info", () -> {
            Camera.CameraInfo b = new Camera.CameraInfo(), f = new Camera.CameraInfo();
            Camera.getCameraInfo(0, b);
            Camera.getCameraInfo(1, f);
            Log.i(T, "info cameras=" + Camera.getNumberOfCameras() + " back=" + b.facing + "/" + b.orientation + " front=" + f.facing + "/" + f.orientation);
            boolean refused = false;
            try {
                Camera.getCameraInfo(5, b);
            } catch (RuntimeException e) {
                refused = "Unknown camera ID".equals(e.getMessage());
            }
            return Camera.getNumberOfCameras() == 2 && b.facing == Camera.CameraInfo.CAMERA_FACING_BACK && b.orientation == 90
                    && f.facing == Camera.CameraInfo.CAMERA_FACING_FRONT && f.orientation == 270 && refused;
        });
        step("open", () -> {
            back = onLooper(Camera::open);
            String bad = "";
            try {
                onLooper(() -> Camera.open(9));
            } catch (RuntimeException e) {
                bad = e.getMessage();
            }
            return back != null && "Fail to connect to camera service".equals(bad);
        });
        if (back == null) {
            Log.i(T, "done ok=" + ok + " failed=" + failed);
            return;
        }
        step("busy", () -> {
            try {
                onLooper(() -> Camera.open(0)).release();
                return false;
            } catch (RuntimeException e) {
                return "Fail to connect to camera service".equals(e.getMessage());
            }
        });
        step("parameters", this::parameters);
        step("orientation", () -> {
            back.setDisplayOrientation(90);
            try {
                back.setDisplayOrientation(45);
                return false;
            } catch (RuntimeException e) {
                return true;
            }
        });

        /*
         * Buffers added before the callback: A, then one too small, then B and C. Frames go into A, then the small one is
         * taken off and the frame given as null (buffers remain), then B, then C, and none after: Android's JNI does that.
         */
        final int size = 320 * 240 * 3 / 2;
        final byte[] a = new byte[size], small = new byte[100], b = new byte[size], c = new byte[size + 64];
        final Frames withBuffer = new Frames(320, 240, 0);
        step("buffers", () -> {
            back.addCallbackBuffer(a);
            back.addCallbackBuffer(small);
            back.addCallbackBuffer(b);
            back.addCallbackBuffer(c);
            back.setPreviewCallbackWithBuffer(withBuffer);
            if (!surfaceReady.await(30, TimeUnit.SECONDS)) return false;
            back.setPreviewDisplay(surfaceView.getHolder());
            back.startPreview();
            boolean four = withBuffer.await(4, 20000);
            Thread.sleep(500);
            Log.i(T, "buffers got=" + withBuffer.count() + " numbers=" + withBuffer.numbers + " threads=" + withBuffer.threads);
            synchronized (withBuffer) {
                return four && withBuffer.count() == 4 && withBuffer.arrays.get(0) == a && withBuffer.arrays.get(1) == null && withBuffer.arrays.get(2) == b
                        && withBuffer.arrays.get(3) == c && withBuffer.allRight && withBuffer.rising() && withBuffer.allOn("CameraLooper");
            }
        });
        /* The preview on the SurfaceView, for the test to look at; a tap goes on. */
        step("preview", () -> {
            Log.i(T, "preview running");
            boolean seen = tapped.await(120, TimeUnit.SECONDS);
            tapped = new CountDownLatch(1);
            return seen;
        });
        step("autofocus", () -> {
            Focus f = new Focus(), cancelled = new Focus();
            back.autoFocus(f);
            boolean answered = f.done.await(10, TimeUnit.SECONDS);
            back.autoFocus(cancelled);
            back.cancelAutoFocus();
            Thread.sleep(500);
            Log.i(T, "autofocus success=" + f.success + " calls=" + f.calls + " thread=" + f.thread + " cancelled=" + cancelled.calls);
            return answered && f.success && f.calls == 1 && "CameraLooper".equals(f.thread) && cancelled.calls == 0;
        });
        step("torch", () -> {
            Camera.Parameters p = back.getParameters();
            p.setFlashMode(Camera.Parameters.FLASH_MODE_TORCH);
            back.setParameters(p);
            Log.i(T, "torch on: " + back.getParameters().getFlashMode());
            Thread.sleep(200);
            p.setFlashMode(Camera.Parameters.FLASH_MODE_OFF);
            back.setParameters(p);
            return Camera.Parameters.FLASH_MODE_OFF.equals(back.getParameters().getFlashMode());
        });
        /* setPreviewCallback: a new array for each frame; one at a time, so that a slow callback misses frames rather than queueing them. */
        step("callback", () -> {
            Frames each = new Frames(320, 240, 150);
            back.setPreviewCallback(each);
            boolean five = each.await(5, 20000);
            back.setPreviewCallback(null);
            Thread.sleep(400);
            int n = each.count();
            Thread.sleep(400);
            boolean distinct, gaps = false;
            synchronized (each) {
                distinct = each.arrays.size() == new java.util.IdentityHashMap<byte[], Boolean>() {{ for (byte[] x : each.arrays) put(x, Boolean.TRUE); }}.size();
                for (int i = 1; i < each.numbers.size(); i++) if (each.numbers.get(i) - each.numbers.get(i - 1) > 1) gaps = true;
            }
            Log.i(T, "callback got=" + n + " numbers=" + each.numbers);
            return five && each.count() == n && distinct && gaps && each.allRight && each.rising() && each.allOn("CameraLooper") && !each.arrays.contains(null);
        });
        step("one-shot", () -> {
            Frames once = new Frames(320, 240, 0);
            back.setOneShotPreviewCallback(once);
            boolean one = once.await(1, 10000);
            Thread.sleep(600);
            return one && once.count() == 1 && once.allRight;
        });
        step("stop", () -> {
            back.stopPreview();
            Frames after = new Frames(320, 240, 0);
            back.setPreviewCallbackWithBuffer(after);
            back.addCallbackBuffer(new byte[size]);
            Thread.sleep(600);
            boolean none = after.count() == 0;
            back.startPreview();
            boolean again = after.await(1, 10000);
            back.stopPreview();
            return none && again && after.allRight;
        });
        step("release", () -> {
            back.release();
            back.release();
            String used = "";
            try {
                back.getParameters();
            } catch (RuntimeException e) {
                used = e.getMessage();
            }
            Camera again = onLooper(() -> Camera.open(0));
            again.release();
            return used.contains("release()");
        });
        /* The front camera, opened on this thread, which has no looper: its callbacks come on the main one. Fixed focus, no light. */
        step("front", () -> {
            Camera front = Camera.open(1);
            Camera.Parameters p = front.getParameters();
            Log.i(T, "front focus=" + p.getSupportedFocusModes() + " flash=" + p.getSupportedFlashModes() + " sizes=" + p.get("preview-size-values"));
            boolean params = has(p.getSupportedFocusModes(), "fixed") && p.getSupportedFocusModes().size() == 1 && p.getSupportedFlashModes() == null
                    && p.getFlashMode() == null && hasSize(p.getSupportedPreviewSizes(), 320, 240);
            p.setPreviewSize(320, 240);
            front.setParameters(p);
            front.setDisplayOrientation(90);
            if (!textureReady.await(30, TimeUnit.SECONDS)) return false;
            front.setPreviewTexture(textureView.getSurfaceTexture());
            Frames frames = new Frames(320, 240, 0);
            front.setPreviewCallbackWithBuffer(frames);
            front.addCallbackBuffer(new byte[size]);
            front.startPreview();
            boolean got = frames.await(1, 20000);
            Focus f = new Focus();
            front.autoFocus(f);
            boolean focused = f.done.await(5, TimeUnit.SECONDS);
            Log.i(T, "front running frame=" + got + " thread=" + frames.threads + " focus=" + f.success + " on " + f.thread);
            boolean seen = tapped.await(120, TimeUnit.SECONDS);
            front.stopPreview();
            front.release();
            return params && got && frames.allRight && frames.allOn("main") && focused && f.success && "main".equals(f.thread) && seen;
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }

    /** The back camera's parameters as an app reads and sets them. */
    private boolean parameters() {
        Camera.Parameters p = back.getParameters();
        Log.i(T, "parameters sizes=" + p.get("preview-size-values") + " fps=" + p.get("preview-fps-range-values") + " focus=" + p.get("focus-mode-values")
                + " flash=" + p.get("flash-mode-values"));
        boolean offered = hasSize(p.getSupportedPreviewSizes(), 1280, 720) && hasSize(p.getSupportedPreviewSizes(), 640, 480) && hasSize(p.getSupportedPreviewSizes(), 320, 240)
                && p.getPreviewFormat() == ImageFormat.NV21 && p.getSupportedPreviewFormats().contains(ImageFormat.NV21) && p.getSupportedPreviewFormats().contains(ImageFormat.YV12)
                && has(p.getSupportedFocusModes(), "auto", "continuous-picture", "continuous-video", "macro") && has(p.getSupportedFlashModes(), "off", "torch")
                && "off".equals(p.getFlashMode()) && !p.isZoomSupported() && p.getMaxZoom() == 0 && p.getZoomRatios() == null && p.getMaxExposureCompensation() == 0
                && p.getMinExposureCompensation() == 0 && p.getExposureCompensation() == 0 && p.getMaxNumFocusAreas() == 0 && p.getMaxNumMeteringAreas() == 0
                && !p.isVideoStabilizationSupported() && has(p.getSupportedSceneModes(), "auto") && has(p.getSupportedWhiteBalance(), "auto")
                && has(p.getSupportedColorEffects(), "none") && p.get("preview-size-values").contains("320x240");
        boolean fps = false;
        for (int[] r : p.getSupportedPreviewFpsRange()) fps |= r[0] == 15000 && r[1] == 30000;
        /* flatten and unflatten give the same map back; an area is written as Android writes it. */
        Camera.Parameters q = back.getParameters();
        q.unflatten(p.flatten());
        List<Camera.Area> areas = new ArrayList<>();
        areas.add(new Camera.Area(new Rect(-100, -100, 100, 100), 1000));
        q.setFocusAreas(areas);
        boolean flat = q.get("focus-areas").equals("(-100,-100,100,100,1000)") && q.getFocusAreas().equals(areas) && q.flatten().startsWith(p.flatten());
        /* Set as a scanner sets them, then read back. */
        p.setPreviewSize(320, 240);
        p.setPreviewFormat(ImageFormat.NV21);
        p.setPreviewFpsRange(15000, 30000);
        p.setFocusMode(Camera.Parameters.FOCUS_MODE_AUTO);
        p.setSceneMode(Camera.Parameters.SCENE_MODE_AUTO);
        p.setWhiteBalance(Camera.Parameters.WHITE_BALANCE_AUTO);
        p.setColorEffect(Camera.Parameters.EFFECT_NONE);
        p.setExposureCompensation(0);
        p.setVideoStabilization(false);
        p.setRecordingHint(true);
        back.setParameters(p);
        Camera.Size s = back.getParameters().getPreviewSize();
        boolean set = s.width == 320 && s.height == 240 && "true".equals(back.getParameters().get("recording-hint"));
        /* What the camera does not offer is refused, as a camera's driver refuses it; a format Android has no name for, by Java. */
        int refusals = 0;
        Camera.Parameters r = back.getParameters();
        r.setPreviewSize(123, 45);
        try {
            back.setParameters(r);
        } catch (RuntimeException e) {
            if ("setParameters failed".equals(e.getMessage())) refusals++;
        }
        r = back.getParameters();
        r.setZoom(2);
        try {
            back.setParameters(r);
        } catch (RuntimeException e) {
            if ("setParameters failed".equals(e.getMessage())) refusals++;
        }
        try {
            r.setPreviewFormat(12345);
        } catch (IllegalArgumentException e) {
            refusals++;
        }
        s = back.getParameters().getPreviewSize();
        Log.i(T, "parameters offered=" + offered + " fps=" + fps + " flat=" + flat + " set=" + set + " refusals=" + refusals);
        return offered && fps && flat && set && refusals == 3 && s.width == 320;
    }
}
