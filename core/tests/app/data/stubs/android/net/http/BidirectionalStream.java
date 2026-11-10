package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class BidirectionalStream {
    public abstract void start();
    public abstract void read(java.nio.ByteBuffer buffer);
    public abstract void write(java.nio.ByteBuffer buffer, boolean endOfStream);
    public abstract void flush();
    public abstract void cancel();
    public abstract boolean isDone();
    public abstract String getHttpMethod();
    public abstract static class Builder {
        public abstract Builder setHttpMethod(String method);
        public abstract Builder addHeader(String header, String value);
        public abstract Builder setPriority(int priority);
        public abstract Builder setDelayRequestHeadersUntilFirstFlushEnabled(boolean delay);
        public abstract BidirectionalStream build();
    }
    public interface Callback {
        void onStreamReady(BidirectionalStream stream);
        void onResponseHeadersReceived(BidirectionalStream stream, UrlResponseInfo info);
        void onReadCompleted(BidirectionalStream stream, UrlResponseInfo info, java.nio.ByteBuffer buffer, boolean endOfStream);
        void onWriteCompleted(BidirectionalStream stream, UrlResponseInfo info, java.nio.ByteBuffer buffer, boolean endOfStream);
        void onResponseTrailersReceived(BidirectionalStream stream, UrlResponseInfo info, HeaderBlock trailers);
        void onSucceeded(BidirectionalStream stream, UrlResponseInfo info);
        void onFailed(BidirectionalStream stream, UrlResponseInfo info, HttpException error);
        void onCanceled(BidirectionalStream stream, UrlResponseInfo info);
    }
}
