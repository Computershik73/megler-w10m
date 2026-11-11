// Overlay of core/framework/gen-src/android/media/MediaScannerConnection.java (tools/build-framework.sh), which had the
// platform's signatures alone: a scan did nothing and its listener was never called. Now as Android's: each file is indexed
// by the media provider (megler.MediaProvider.scanPath) on the provider's own thread -- Android's scanner runs in its own
// process and calls back on a binder thread, never the caller's -- and the listener hears it there with the file's item URI
// (content://media/external/images/media/<id> and the like), or null for a file that is not there, is hidden or is not in
// the shared storage. connect() is answered with onMediaScannerConnected there too; scanFile before it is Android's
// IllegalStateException.
//
// Why. "Save to gallery" in an app written before Android 10 is a file written to /sdcard/DCIM or Pictures and then this
// (or the broadcast ACTION_MEDIA_SCANNER_SCAN_FILE, megler.Broadcasts): the app waits for the URI to share or show the
// file, and some wait for the listener before they say it was saved.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class MediaScannerConnection implements android.content.ServiceConnection {
    private final android.content.Context mContext;
    private final MediaScannerConnectionClient mClient;
    private boolean mConnected;

    public MediaScannerConnection(android.content.Context context, MediaScannerConnectionClient client) {
        mContext = context;
        mClient = client;
    }

    public static void scanFile(android.content.Context context, String[] paths, String[] mimeTypes, final OnScanCompletedListener callback) {
        if (paths == null) return;
        megler.MediaProvider.scanLater(paths.clone(), (path, uri) -> { if (callback != null) callback.onScanCompleted(path, uri); });
    }

    public void connect() {
        synchronized (this) {
            if (mConnected) return;
            mConnected = true;
        }
        if (mClient != null) megler.MediaProvider.later(() -> mClient.onMediaScannerConnected());
    }

    public void disconnect() {
        synchronized (this) { mConnected = false; }
    }

    public synchronized boolean isConnected() { return mConnected; }

    public void scanFile(String path, String mimeType) {
        synchronized (this) {
            if (!mConnected) throw new IllegalStateException("not connected to MediaScannerService");
        }
        megler.MediaProvider.scanLater(new String[] { path }, (p, uri) -> { if (mClient != null) mClient.onScanCompleted(p, uri); });
    }

    public void onServiceConnected(android.content.ComponentName name, android.os.IBinder service) {}
    public void onServiceDisconnected(android.content.ComponentName name) {}

    MediaScannerConnection() { this((android.content.Context) null, (MediaScannerConnectionClient) null); }

    public interface MediaScannerConnectionClient extends OnScanCompletedListener {
        void onMediaScannerConnected();
    }
    public interface OnScanCompletedListener {
        void onScanCompleted(String path, android.net.Uri uri);
    }
}
