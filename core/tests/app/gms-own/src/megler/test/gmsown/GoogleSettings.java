package megler.test.gmsown;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.util.Log;

import java.util.HashMap;

/**
 * Google's settings as Google Services Framework's GoogleSettingsProvider keeps them (content://com.google.settings/partner):
 * a value for each name, read by a query of "name=?" and written by an insert of the name and the value, each insert said in
 * the log. Held in memory, as long as the app runs.
 */
public class GoogleSettings extends ContentProvider {
    private final HashMap<String, String> values = new HashMap<>();

    @Override
    public boolean onCreate() { return true; }

    @Override
    public synchronized Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        /* Who asks, said back (content://com.google.settings/caller): the hosting app's provider asks so from inside a call
           that Google Play services' own code made into it (gms-host's WhoAsks), and it is the app that asks then. */
        if (args == null && "caller".equals(uri.getLastPathSegment())) {
            MatrixCursor who = new MatrixCursor(new String[] { "said" });
            who.addRow(new Object[] { "caller " + getCallingPackage() + " uid " + Binder.getCallingUid() });
            return who;
        }
        MatrixCursor c = new MatrixCursor(new String[] { "value" });
        String name = args != null && args.length > 0 ? args[0] : uri.getLastPathSegment();
        if (name != null && values.containsKey(name)) c.addRow(new Object[] { values.get(name) });
        Log.i("GmsOwnTest", "settings read " + name + "=" + values.get(name));
        return c;
    }

    @Override
    public synchronized Uri insert(Uri uri, ContentValues v) {
        String name = v.getAsString("name"), value = v.getAsString("value");
        values.put(name, value);
        Log.i("GmsOwnTest", "settings " + name + "=" + value);
        return Uri.withAppendedPath(uri, name);
    }

    @Override
    public String getType(Uri uri) { return "vnd.android.cursor.dir/partner"; }

    @Override
    public int delete(Uri uri, String selection, String[] args) { return 0; }

    @Override
    public int update(Uri uri, ContentValues v, String selection, String[] args) { return 0; }
}
