package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class NetworkException extends HttpException {
    public static final int ERROR_HOSTNAME_NOT_RESOLVED = 1, ERROR_CONNECTION_CLOSED = 5, ERROR_CONNECTION_REFUSED = 7, ERROR_OTHER = 11;
    public NetworkException(String message, Throwable cause) { super(message, cause); }
    public abstract int getErrorCode();
    public abstract boolean isImmediatelyRetryable();
}
