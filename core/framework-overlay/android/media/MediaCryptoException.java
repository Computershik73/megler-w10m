// Overlay of core/framework/gen-src/android/media/MediaCryptoException.java (tools/build-framework.sh).
// It differs in keeping what it is made with: the generated stub's constructors dropped the message (getMessage() was
// null) and the three error numbers; here they are Android's.
//
// Why. MediaCrypto's constructor throws one (MediaCrypto.java), and what an app logs or shows of it is its message.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaCryptoException extends java.lang.Exception implements android.media.MediaDrmThrowable {
    private final int mVendorError, mOemError, mErrorContext;
    public MediaCryptoException(java.lang.String detailMessage) { this(detailMessage, 0, 0, 0); }
    public MediaCryptoException(java.lang.String message, int vendorError, int oemError, int errorContext) {
        super(message);
        mVendorError = vendorError;
        mOemError = oemError;
        mErrorContext = errorContext;
    }
    @Override public int getErrorContext() { return mErrorContext; }
    @Override public int getOemError() { return mOemError; }
    @Override public int getVendorError() { return mVendorError; }
}
