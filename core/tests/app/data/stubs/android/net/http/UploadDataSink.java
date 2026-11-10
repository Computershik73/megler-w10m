package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): Android 14's class. */
public abstract class UploadDataSink {
    public abstract void onReadSucceeded(boolean finalChunk);
    public abstract void onReadError(Exception exception);
    public abstract void onRewindSucceeded();
    public abstract void onRewindError(Exception exception);
}
