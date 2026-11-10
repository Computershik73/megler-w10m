package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): Android 14's class. */
public abstract class CallbackException extends HttpException {
    protected CallbackException(String message, Throwable cause) { super(message, cause); }
}
