// Overlay of core/framework/gen-src/android/media/MediaDrmResetException.java (tools/build-framework.sh).
// It differs in keeping its message, which the generated stub's constructor dropped (getMessage() was null), as
// MediaDrmException.java beside it does.
//
// Why. An app's own code makes and reports these (a player's DRM session layer); what it logs of one is its message.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class MediaDrmResetException extends java.lang.IllegalStateException implements android.media.MediaDrmThrowable {
    public MediaDrmResetException(java.lang.String detailMessage) { super(detailMessage); }
}
