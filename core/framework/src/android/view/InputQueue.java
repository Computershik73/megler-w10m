package android.view;

/**
 * A window's input given to native code (Window.takeInputQueue): its native object is the core's AInputQueue
 * (core/src/ndk/mg_ndk.c), which NativeActivity hands its native code (onInputQueueCreated). The window's touches and keys are
 * put into it instead of being dispatched to its views (meglerSend, from megler.DecorView); a key native code finishes
 * unhandled goes on to the window's callback, the activity, as Android's ViewRootImpl hands it to the next stage
 * (meglerUnhandledKey, from the core).
 */
public final class InputQueue {
    public interface Callback { void onInputQueueCreated(InputQueue q); void onInputQueueDestroyed(InputQueue q); }
    public interface FinishedInputEventCallback { void onFinishedInputEvent(Object token, boolean handled); }

    private long mPtr;
    private final Window mWindow;

    private static native long nativeInit(InputQueue owner);
    private static native void nativeDispose(long ptr);
    private static native void nativeTouch(long ptr, int phase, int pointerId, float x, float y);
    private static native void nativeKey(long ptr, int keyCode, boolean down);

    public InputQueue() { this(null); }
    /** Megler: the queue of a window's input, whose unhandled keys go to that window's callback. */
    public InputQueue(Window w) { mWindow = w; mPtr = nativeInit(this); }
    public void dispose() { dispose(false); }
    public void dispose(boolean finalized) { if (mPtr != 0) { nativeDispose(mPtr); mPtr = 0; } }
    public long getNativePtr() { return mPtr; }
    public void sendInputEvent(InputEvent e, Object token, boolean predispatch, FinishedInputEventCallback callback) {
        meglerSend(e);
        if (callback != null) callback.onFinishedInputEvent(token, true);
    }

    /**
     * Megler: an event of the window's, put into the queue as the core's touches and keys (0 down, 1 move, 2 up, 3 cancel, by
     * pointer id; a key down or up), in the window's coordinates. True when it was taken.
     */
    public boolean meglerSend(InputEvent e) {
        long p = mPtr;
        if (p == 0) return false;
        if (e instanceof KeyEvent) {
            KeyEvent k = (KeyEvent) e;
            if (k.getAction() != KeyEvent.ACTION_DOWN && k.getAction() != KeyEvent.ACTION_UP) return true;
            nativeKey(p, k.getKeyCode(), k.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        if (!(e instanceof MotionEvent)) return false;
        MotionEvent m = (MotionEvent) e;
        int i = m.getActionIndex();
        switch (m.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: nativeTouch(p, 0, m.getPointerId(i), m.getX(i), m.getY(i)); break;
            case MotionEvent.ACTION_MOVE: for (int n = 0; n < m.getPointerCount(); n++) nativeTouch(p, 1, m.getPointerId(n), m.getX(n), m.getY(n)); break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: nativeTouch(p, 2, m.getPointerId(i), m.getX(i), m.getY(i)); break;
            case MotionEvent.ACTION_CANCEL: nativeTouch(p, 3, m.getPointerId(0), m.getX(0), m.getY(0)); break;
            default: return false;
        }
        return true;
    }

    /** Megler: a key native code finished without handling it (AInputQueue_finishEvent), from its thread: to the window's callback on the main thread. */
    private void meglerUnhandledKey(final int keyCode, final boolean down) {
        final Window w = mWindow;
        if (w == null) return;
        megler.ViewRoot.mainHandler().post(() -> {
            Window.Callback cb = w.getCallback();
            if (cb == null || w.isDestroyed()) return;
            long t = android.os.SystemClock.uptimeMillis();
            cb.dispatchKeyEvent(new KeyEvent(t, t, down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, keyCode, 0));
        });
    }
}
