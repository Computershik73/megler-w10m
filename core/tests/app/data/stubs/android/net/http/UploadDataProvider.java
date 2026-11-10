package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): Android 14's class. */
public abstract class UploadDataProvider implements java.io.Closeable {
    public abstract long getLength() throws java.io.IOException;
    public abstract void read(UploadDataSink uploadDataSink, java.nio.ByteBuffer byteBuffer) throws java.io.IOException;
    public abstract void rewind(UploadDataSink uploadDataSink) throws java.io.IOException;
    public void close() throws java.io.IOException {}
}
