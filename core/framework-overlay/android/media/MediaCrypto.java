// Overlay of core/framework/gen-src/android/media/MediaCrypto.java (tools/build-framework.sh).
// It differs in saying that there is none, as MediaDrm.java beside it does: Megler decrypts no protected media
// (MediaCodec.queueSecureInputBuffer refuses a protected sample), so no scheme is supported -- isCryptoSchemeSupported is
// false, as it was -- and the constructor, which made an object that did nothing, throws
// android.media.MediaCryptoException ("Failed to instantiate crypto object."), as Android's does for a scheme the device
// has no plugin for. Everything else is the generated stub's -- out of reach, with no instance to call it on.
//
// Why. A decoder configured with a MediaCrypto is promised decryption; an app that could make one went on to feed it
// encrypted samples.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class MediaCrypto {
    /** There is no scheme to make one of. */
    public MediaCrypto(java.util.UUID uuid, byte[] sessionId) throws android.media.MediaCryptoException {
        if (uuid == null) throw new NullPointerException("uuid");
        throw new android.media.MediaCryptoException("Failed to instantiate crypto object.");
    }
    public static boolean isCryptoSchemeSupported(java.util.UUID uuid) { return false; }
    protected void finalize() {}
    public void release() {}
    public boolean requiresSecureDecoderComponent(java.lang.String mime) { return false; }
    public void setMediaDrmSession(byte[] sessionId) throws android.media.MediaCryptoException {}
}
