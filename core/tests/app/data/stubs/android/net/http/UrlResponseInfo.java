package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine* call of Android 14's class. */
public abstract class UrlResponseInfo {
    public abstract HeaderBlock getHeaders();
    public abstract int getHttpStatusCode();
    public abstract String getHttpStatusText();
    public abstract String getNegotiatedProtocol();
    public abstract long getReceivedByteCount();
    public abstract String getUrl();
    public abstract java.util.List<String> getUrlChain();
    public abstract boolean wasCached();
}
