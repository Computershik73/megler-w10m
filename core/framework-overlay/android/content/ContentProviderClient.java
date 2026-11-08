// Overlay of core/framework/src/android/content/ContentProviderClient.java (tools/build-framework.sh).
// It differs in its files and the platform's newer calls: openFile, openAssetFile, openTypedAssetFile(Descriptor), query with
// a Bundle, insert, update and delete with extras, bulkInsert, call with an authority, getStreamTypes, canonicalize,
// uncanonicalize and refresh go to the provider, as the client's other calls do; they were generated signatures answering
// null, 0 or false.
//
// Why. Google Play services' DynamiteLoader reads the Chimera configuration of its modules through the client:
// acquireUnstableContentProviderClient("content://com.google.android.gms.chimera/api/<n>").openFile(uri, "r"). With null it
// gave up, "Failed to create module context. java.io.IOException: Config file descriptor is null", for every module it loads
// so (loadModule2NoCrashUtils): the Google Maps module's own Google certificates among them ("Failed to get Google
// certificates from remote", Transit, 0.1.0.671). Everything else is the original's, unchanged.
package android.content;
import android.database.Cursor;
import android.net.Uri;
public class ContentProviderClient implements AutoCloseable {
    private final ContentProvider p;
    ContentProviderClient(ContentProvider p) { this.p = p; }
    public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort) { return p.query(u, proj, sel, args, sort); }
    public Uri insert(Uri u, ContentValues v) { return p.insert(u, v); }
    public int update(Uri u, ContentValues v, String sel, String[] args) { return p.update(u, v, sel, args); }
    public int delete(Uri u, String sel, String[] args) { return p.delete(u, sel, args); }
    public String getType(Uri u) { return p.getType(u); }
    public android.os.Bundle call(String method, String arg, android.os.Bundle extras) { return p.call(method, arg, extras); }
    public ContentProvider getLocalContentProvider() { return p; }
    public ContentProviderResult[] applyBatch(java.util.ArrayList<ContentProviderOperation> ops) throws OperationApplicationException { return p.applyBatch(ops); }
    public ContentProviderResult[] applyBatch(String authority, java.util.ArrayList<ContentProviderOperation> ops) throws OperationApplicationException { return p.applyBatch(authority, ops); }
    public boolean release() { return true; }
    public void close() {}
    // ---- Megler: the provider's files and its newer calls (see the top of the file)
    public static void closeQuietly(android.content.ContentProviderClient p0) {}
    public static void releaseQuietly(android.content.ContentProviderClient p0) {}
    public int bulkInsert(android.net.Uri p0, android.content.ContentValues[] p1) { return p.bulkInsert(p0, p1); }
    public android.os.Bundle call(java.lang.String p0, java.lang.String p1, java.lang.String p2, android.os.Bundle p3) { return p.call(p0, p1, p2, p3); }
    public android.net.Uri canonicalize(android.net.Uri p0) { return p.canonicalize(p0); }
    public int checkUriPermission(android.net.Uri p0, int p1, int p2) { return 0; }
    public int delete(android.net.Uri p0, android.os.Bundle p1) { return p.delete(p0, p1); }
    public java.lang.String[] getStreamTypes(android.net.Uri p0, java.lang.String p1) { return p.getStreamTypes(p0, p1); }
    public android.net.Uri insert(android.net.Uri p0, android.content.ContentValues p1, android.os.Bundle p2) { return p.insert(p0, p1, p2); }
    public android.content.res.AssetFileDescriptor openAssetFile(android.net.Uri p0, java.lang.String p1) throws java.io.FileNotFoundException { return p.openAssetFile(p0, p1); }
    public android.content.res.AssetFileDescriptor openAssetFile(android.net.Uri p0, java.lang.String p1, android.os.CancellationSignal p2) throws java.io.FileNotFoundException { return p.openAssetFile(p0, p1, p2); }
    public android.os.ParcelFileDescriptor openFile(android.net.Uri p0, java.lang.String p1) throws java.io.FileNotFoundException { return p.openFile(p0, p1); }
    public android.os.ParcelFileDescriptor openFile(android.net.Uri p0, java.lang.String p1, android.os.CancellationSignal p2) throws java.io.FileNotFoundException { return p.openFile(p0, p1, p2); }
    public android.content.res.AssetFileDescriptor openTypedAssetFile(android.net.Uri p0, java.lang.String p1, android.os.Bundle p2, android.os.CancellationSignal p3) throws java.io.FileNotFoundException { return p.openTypedAssetFile(p0, p1, p2); }
    public android.content.res.AssetFileDescriptor openTypedAssetFileDescriptor(android.net.Uri p0, java.lang.String p1, android.os.Bundle p2) throws java.io.FileNotFoundException { return p.openTypedAssetFile(p0, p1, p2); }
    public android.content.res.AssetFileDescriptor openTypedAssetFileDescriptor(android.net.Uri p0, java.lang.String p1, android.os.Bundle p2, android.os.CancellationSignal p3) throws java.io.FileNotFoundException { return p.openTypedAssetFile(p0, p1, p2); }
    public android.database.Cursor query(android.net.Uri p0, java.lang.String[] p1, android.os.Bundle p2, android.os.CancellationSignal p3) { return p.query(p0, p1, p2, p3); }
    public android.database.Cursor query(android.net.Uri p0, java.lang.String[] p1, java.lang.String p2, java.lang.String[] p3, java.lang.String p4, android.os.CancellationSignal p5) { return p.query(p0, p1, p2, p3, p4, p5); }
    public boolean refresh(android.net.Uri p0, android.os.Bundle p1, android.os.CancellationSignal p2) { return p.refresh(p0, p1, p2); }
    public void setDetectNotResponding(long p0) {}
    public android.net.Uri uncanonicalize(android.net.Uri p0) { return p.uncanonicalize(p0); }
    public int update(android.net.Uri p0, android.content.ContentValues p1, android.os.Bundle p2) { return p.update(p0, p1, p2); }
}
