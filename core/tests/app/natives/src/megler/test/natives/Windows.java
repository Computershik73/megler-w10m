package megler.test.natives;

import android.view.Surface;

/** The class whose native methods are in libwindows.so (jni/windows.c): native code on SurfaceViews' surfaces. */
final class Windows {
    static {
        System.loadLibrary("windows");
    }

    private Windows() {}

    /** The EGL view's surfaceChanged: its window, handed to the renderer (started the first time) when it is a new one. What was found. */
    static native String surfaceChanged(Surface surface, int width, int height);
    /** The EGL view's surfaceDestroyed: the library lets go of the window; the renderer finds out at its next swap. */
    static native void surfaceDestroyed();
    /** How far the renderer has come (jni/windows.c says what each number means). */
    static native int phase();
    /** The renderer ends; waited for. */
    static native void stop();
    /** The lock view's surface drawn in software, in quarters of four colours. What the buffer was. */
    static native String lockPaint(Surface surface);
    /** A TextureView's surface drawn in software, in quarters of the same colours. What the buffer was. */
    static native String textureLock(Surface surface);
    /** The same drawn once with EGL on the calling thread. What the surface was. */
    static native String textureEgl(Surface surface);
    /** Its EGL surface destroyed by the calling thread while another thread has it current and then draws on it. What each step gave. */
    static native String textureEglElsewhere(Surface surface);
}
