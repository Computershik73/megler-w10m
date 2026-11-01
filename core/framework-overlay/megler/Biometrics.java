package megler;

/**
 * The phone's check of its user (Windows Hello: a fingerprint, the iris, a face or the PIN), behind BiometricPrompt,
 * BiometricManager and FingerprintManager (overlaid): asked of the shell (megler_biometric_host, core/src/drivers/mg_java.c).
 * Whether the phone can is asked again after five seconds at the most, as the user may set Windows Hello up meanwhile. A
 * check runs the phone's own dialog on a thread of its own; one at a time, as Android's prompt is: a second, while one runs,
 * is told the phone is busy.
 */
public final class Biometrics {
    public static final int READY = 0, NOT_SET_UP = 1, NONE = 2;
    public static final int VERIFIED = 0, CANCELED = 1, LOCKED = 2, UNAVAILABLE = 3, BUSY = 4;

    /** What a check ends with, on the thread it ran on: one of VERIFIED .. BUSY. */
    public interface Done { void done(int result); }

    private static int sAvailable = -1;
    private static long sAskedAt;
    private static Object sRunning;

    private Biometrics() {}

    public static synchronized int available() {
        long now = android.os.SystemClock.uptimeMillis();
        if (sAvailable < 0 || now - sAskedAt > 5000) {
            sAvailable = Native.biometricAvailable();
            sAskedAt = now;
        }
        return sAvailable;
    }

    /** The message of the phone's dialog: the prompt's title, then its subtitle or description, else the app's name. */
    public static String message(CharSequence title, CharSequence subtitle, CharSequence description) {
        StringBuilder b = new StringBuilder();
        if (title != null && title.length() > 0) b.append(title);
        CharSequence more = description != null && description.length() > 0 ? description : subtitle;
        if (more != null && more.length() > 0) b.append(b.length() > 0 ? "\n" : "").append(more);
        if (b.length() == 0) {
            CharSequence label = null;
            try {
                android.content.Context c = android.app.ActivityThread.currentApplication();
                if (c != null) label = c.getPackageManager().getApplicationLabel(c.getApplicationInfo());
            } catch (Throwable t) {}
            b.append(label != null && label.length() > 0 ? label : Native.packageName());
        }
        return b.toString();
    }

    /**
     * Checks the user with the phone's dialog showing `message`, on a thread of its own; `done` is told how it went, on that
     * thread. `cancel`, when given, closes the dialog when the app cancels (the check then ends CANCELED).
     */
    public static void verify(final String message, final android.os.CancellationSignal cancel, final Done done) {
        final Object me = new Object();
        synchronized (Biometrics.class) {
            if (sRunning != null) {
                done.done(BUSY);
                return;
            }
            sRunning = me;
        }
        Thread t = new Thread(new Runnable() {
            @Override public void run() {
                int r = UNAVAILABLE;
                try {
                    if (cancel != null) cancel.setOnCancelListener(new android.os.CancellationSignal.OnCancelListener() {
                        @Override public void onCancel() { synchronized (Biometrics.class) { if (sRunning == me) Native.biometricCancel(); } }
                    });
                    r = cancel != null && cancel.isCanceled() ? CANCELED : Native.biometricVerify(message);
                    if (cancel != null && cancel.isCanceled()) r = CANCELED;
                } finally {
                    synchronized (Biometrics.class) { if (sRunning == me) sRunning = null; }
                    if (cancel != null) cancel.setOnCancelListener(null);
                }
                done.done(r);
            }
        }, "megler biometrics");
        t.setDaemon(true);
        t.start();
    }
}
