package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class UrlRequest {
    public abstract void start();
    public abstract void followRedirect();
    public abstract void read(java.nio.ByteBuffer buffer);
    public abstract void cancel();
    public abstract boolean isDone();
    public abstract void getStatus(StatusListener listener);
    public abstract String getHttpMethod();
    public abstract HeaderBlock getHeaders();
    public abstract static class Builder {
        public abstract Builder setHttpMethod(String method);
        public abstract Builder addHeader(String header, String value);
        public abstract Builder setUploadDataProvider(UploadDataProvider uploadDataProvider, java.util.concurrent.Executor executor);
        public abstract UrlRequest build();
    }
    public interface Callback {
        void onRedirectReceived(UrlRequest request, UrlResponseInfo info, String newLocationUrl) throws Exception;
        void onResponseStarted(UrlRequest request, UrlResponseInfo info) throws Exception;
        void onReadCompleted(UrlRequest request, UrlResponseInfo info, java.nio.ByteBuffer byteBuffer) throws Exception;
        void onSucceeded(UrlRequest request, UrlResponseInfo info);
        void onFailed(UrlRequest request, UrlResponseInfo info, HttpException error);
        void onCanceled(UrlRequest request, UrlResponseInfo info);
    }
    public static class Status {
        public static final int INVALID = -1, IDLE = 0;
    }
    public interface StatusListener {
        void onStatus(int status);
    }
}
