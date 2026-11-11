package megler.test.pickers;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileNotFoundException;

/**
 * The app's own FileProvider, as androidx's FileProvider is to a camera app: content://megler.test.pickers.files/<name> is
 * <name> in the app's cache, opened as the camera asks -- "w" for the photo it writes there (MediaStore.EXTRA_OUTPUT).
 */
public class Files extends ContentProvider {
    public static final String AUTHORITY = "megler.test.pickers.files";

    static File file(ContentProvider p, Uri u) {
        String name = u.getLastPathSegment();
        if (name == null || name.contains("/") || name.equals("..")) return null;
        return new File(p.getContext().getCacheDir(), name);
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
        File f = file(this, u);
        if (f == null) throw new FileNotFoundException(u.toString());
        Main.heardOpen(mode);
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public Cursor query(Uri u, String[] projection, String selection, String[] args, String sort) {
        File f = file(this, u);
        MatrixCursor m = new MatrixCursor(new String[] { "_display_name", "_size" });
        if (f != null) m.addRow(new Object[] { f.getName(), f.length() });
        return m;
    }

    @Override public String getType(Uri u) { return "image/jpeg"; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String selection, String[] args) { return 0; }
    @Override public int update(Uri u, ContentValues v, String selection, String[] args) { return 0; }
}
