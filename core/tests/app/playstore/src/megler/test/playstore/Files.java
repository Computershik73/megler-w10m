package megler.test.playstore;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;

/**
 * The stand-in Play Store's FileProvider, as com.google.android.finsky.FileProvider is the Play Store's:
 * content://com.android.vending.test.files/<name> is <name> in the app's cache, a downloaded split that the app which
 * asked for it is let read. No other app's process can open it here, which is why Megler reads it in this one.
 */
public class Files extends ContentProvider {
    public static final String AUTHORITY = "com.android.vending.test.files";

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
        String name = u.getLastPathSegment();
        if (name == null || name.contains("/") || name.equals("..") || !"r".equals(mode)) throw new FileNotFoundException(u.toString());
        Log.i(Main.T, "the provider's file " + name + " is opened");
        return ParcelFileDescriptor.open(new File(getContext().getCacheDir(), name), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri u, String[] projection, String selection, String[] args, String sort) {
        String name = u.getLastPathSegment();
        MatrixCursor m = new MatrixCursor(new String[] { "_display_name", "_size" });
        if (name != null && !name.contains("/")) m.addRow(new Object[] { name, new File(getContext().getCacheDir(), name).length() });
        return m;
    }

    @Override public String getType(Uri u) { return "application/vnd.android.package-archive"; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String selection, String[] args) { return 0; }
    @Override public int update(Uri u, ContentValues v, String selection, String[] args) { return 0; }
}
