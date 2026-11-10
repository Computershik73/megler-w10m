package android.net.http;

import java.nio.ByteBuffer;

/**
 * Megler: Android 14's BidirectionalStream, whole: the generated stub this replaces had a Builder with no methods, so an
 * app's builder.setHttpMethod (gRPC's Cronet transport calls it first) had nothing to call. Megler's stream is
 * megler.http.Stream.
 */
public abstract class BidirectionalStream {
    public static final int STREAM_PRIORITY_IDLE = 0;
    public static final int STREAM_PRIORITY_LOWEST = 1;
    public static final int STREAM_PRIORITY_LOW = 2;
    public static final int STREAM_PRIORITY_MEDIUM = 3;
    public static final int STREAM_PRIORITY_HIGHEST = 4;

    public BidirectionalStream() {}

    public abstract String getHttpMethod();

    public abstract boolean hasTrafficStatsTag();

    public abstract int getTrafficStatsTag();

    public abstract boolean hasTrafficStatsUid();

    public abstract int getTrafficStatsUid();

    public abstract HeaderBlock getHeaders();

    public abstract int getPriority();

    public abstract boolean isDelayRequestHeadersUntilFirstFlushEnabled();

    public abstract void start();

    public abstract void read(ByteBuffer buffer);

    public abstract void write(ByteBuffer buffer, boolean endOfStream);

    public abstract void flush();

    public abstract void cancel();

    public abstract boolean isDone();

    public abstract static class Builder {
        public Builder() {}

        public abstract Builder setHttpMethod(String method);

        public abstract Builder addHeader(String header, String value);

        public abstract Builder setPriority(int priority);

        public abstract Builder setDelayRequestHeadersUntilFirstFlushEnabled(boolean delayRequestHeadersUntilFirstFlush);

        public abstract Builder setTrafficStatsTag(int tag);

        public abstract Builder setTrafficStatsUid(int uid);

        public abstract BidirectionalStream build();
    }

    public interface Callback {
        void onStreamReady(BidirectionalStream stream);

        void onResponseHeadersReceived(BidirectionalStream stream, UrlResponseInfo info);

        void onReadCompleted(BidirectionalStream stream, UrlResponseInfo info, ByteBuffer buffer, boolean endOfStream);

        void onWriteCompleted(BidirectionalStream stream, UrlResponseInfo info, ByteBuffer buffer, boolean endOfStream);

        void onResponseTrailersReceived(BidirectionalStream stream, UrlResponseInfo info, HeaderBlock trailers);

        void onSucceeded(BidirectionalStream stream, UrlResponseInfo info);

        void onFailed(BidirectionalStream stream, UrlResponseInfo info, HttpException error);

        void onCanceled(BidirectionalStream stream, UrlResponseInfo info);
    }
}
