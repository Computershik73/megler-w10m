package megler.test.gmshost;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;

/**
 * A provider of the app's that says who asks it, as a provider that answers some callers alone finds out (Files by Google's
 * FolderSharingContentProvider answers Google Play services and no one else, by getCallingPackage; its insert the app
 * alone, by Binder.getCallingUid). One row, one column, of text:
 *  - the caller's package (getCallingPackage) and uid (Binder.getCallingUid, with the name the package manager has for it);
 *  - the uid with Binder's identity cleared, and restored;
 *  - the package and the uid with the provider's identity cleared (ContentProvider.clearCallingIdentity), and restored;
 *  - and who a provider of Google Play services' is told asks it when this one asks it from inside the call (the
 *    stand-in's settings, content://com.google.settings/caller): the app, whoever asked this one.
 * Asked by Google Play services' own code (the stand-in's Probe) the caller is Google Play services; asked by the app (Main),
 * the app.
 */
public class WhoAsks extends ContentProvider {
    public static final Uri URI = Uri.parse("content://megler.test.gmshost.whoasks/caller");

    @Override
    public boolean onCreate() { return true; }

    private String uid(int uid) { return uid + " " + getContext().getPackageManager().getNameForUid(uid); }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        StringBuilder said = new StringBuilder("caller ").append(getCallingPackage()).append(" uid ").append(uid(Binder.getCallingUid()));
        long token = Binder.clearCallingIdentity();
        said.append(", uid cleared ").append(uid(Binder.getCallingUid()));
        Binder.restoreCallingIdentity(token);
        said.append(" restored ").append(uid(Binder.getCallingUid()));
        CallingIdentity identity = clearCallingIdentity();
        said.append(", identity cleared ").append(getCallingPackage()).append(' ').append(uid(Binder.getCallingUid()));
        restoreCallingIdentity(identity);
        said.append(" restored ").append(getCallingPackage()).append(' ').append(uid(Binder.getCallingUid()));
        String inner = null;
        try (Cursor c = getContext().getContentResolver().query(Uri.parse("content://com.google.settings/caller"), null, null, null, null)) {
            if (c != null && c.moveToFirst()) inner = c.getString(0);
        }
        said.append(", their provider asked from inside: ").append(inner).append(", then ").append(getCallingPackage()).append(' ').append(uid(Binder.getCallingUid()));
        MatrixCursor answer = new MatrixCursor(new String[] { "said" });
        answer.addRow(new Object[] { said.toString() });
        return answer;
    }

    @Override
    public String getType(Uri uri) { return "vnd.android.cursor.item/caller"; }

    @Override
    public Uri insert(Uri uri, ContentValues v) { return null; }

    @Override
    public int delete(Uri uri, String selection, String[] args) { return 0; }

    @Override
    public int update(Uri uri, ContentValues v, String selection, String[] args) { return 0; }
}
