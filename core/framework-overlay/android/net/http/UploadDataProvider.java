package android.net.http;

import java.io.IOException;
import java.nio.ByteBuffer;

/** Megler: Android 14's UploadDataProvider, written: close() does nothing unless a provider has something to close, as on
 *  Android (in the generated stub it was a signature alone, said as such when an engine closed an app's provider). */
public abstract class UploadDataProvider implements java.io.Closeable {
    public UploadDataProvider() {}

    public abstract long getLength() throws IOException;

    public abstract void read(UploadDataSink uploadDataSink, ByteBuffer byteBuffer) throws IOException;

    public abstract void rewind(UploadDataSink uploadDataSink) throws IOException;

    @Override
    public void close() throws IOException {}
}
