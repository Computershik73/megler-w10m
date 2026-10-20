// Overlay: a class of this framework's own, new (the original has no camera). The natives behind
// android.hardware.Camera (the overlay's), which core/src/drivers/mg_java_camera.c answers over the shell's cameras
// (megler_camera_host in core/include/megler.h: Windows.Media.Capture on a phone).
package megler;

/**
 * The phone's cameras. A camera is a handle from open(); start() has its pictures come, and waitNews() waits for the next
 * news of it on the thread that takes the pictures: a picture held (copyFrame and render read it until the next), a focus
 * that ended, the camera gone.
 */
public final class CameraNative {
    private CameraNative() {}
    public static final int NEWS_FRAME = 1, NEWS_FOCUSED = 2, NEWS_NOT_FOCUSED = 4, NEWS_GONE = 8;
    public static final int FOCUS_NONE = 0, FOCUS_ONCE = 1, FOCUS_MACRO = 2, FOCUS_CONTINUOUS = 3;
    public static final int FEATURE_ANY = 1, FEATURE_BACK = 2, FEATURE_FRONT = 4, FEATURE_AUTOFOCUS = 8, FEATURE_FLASH = 16;
    public static native int count();
    /** out = { facing, orientation, focus, light }, the last two 1, 0, or -1 for not known before the camera was opened once. */
    public static native boolean info(int index, int[] out);
    public static native int features();
    /** 0 when it cannot be opened; the log says why. */
    public static native long open(int index);
    /** { focus, macro, torch, n, then width, height, min fps, max fps of each of the n sizes, the largest first }. */
    public static native int[] caps(long h);
    public static native boolean start(long h, int width, int height, int fps);
    public static native void stop(long h);
    public static native int waitNews(long h, int ms);
    /** The picture held: width << 16 | height. */
    public static native int frameSize(long h);
    /** Bytes of a picture of this size in an ImageFormat (NV21, YV12). */
    public static native int frameBytes(int width, int height, int format);
    /** The picture held into dst in an ImageFormat (NV21, YV12): the bytes written, -1 when dst is too small. */
    public static native int copyFrame(long h, byte[] dst, int format);
    /** The picture held into dst, mirrored first if asked and turned `rotation` degrees clockwise: dst is of the turned size. */
    public static native boolean render(long h, android.graphics.Bitmap dst, int rotation, boolean mirror);
    public static native boolean focus(long h, int mode);
    public static native void torch(long h, boolean on);
    public static native void close(long h);
}
