package megler;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.provider.Settings;
import java.util.List;

/** The system, secure and global settings URIs used by Google Services Framework. */
public final class SettingsProvider extends ContentProvider {
    public static final SettingsProvider INSTANCE = new SettingsProvider();
    private SettingsProvider() {}
    @Override public boolean onCreate() { return true; }

    private static String table(Uri uri) {
        List<String> parts = uri.getPathSegments();
        if (parts.isEmpty()) return null;
        String name = parts.get(0);
        return "system".equals(name) || "secure".equals(name) || "global".equals(name) ? name : null;
    }
    private static String key(Uri uri, String selection, String[] args) {
        List<String> parts = uri.getPathSegments();
        if (parts.size() > 1) return parts.get(1);
        return "name=?".equals(selection) && args != null && args.length > 0 ? args[0] : null;
    }
    private static String get(String table, String key) {
        if ("system".equals(table)) return Settings.System.getString(null, key);
        if ("secure".equals(table)) return Settings.Secure.getString(null, key);
        return Settings.Global.getString(null, key);
    }
    private static boolean put(String table, String key, String value) {
        if ("system".equals(table)) return Settings.System.putString(null, key, value);
        if ("secure".equals(table)) return Settings.Secure.putString(null, key, value);
        return Settings.Global.putString(null, key, value);
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        String[] columns = projection == null ? new String[] {"name", "value"} : projection;
        MatrixCursor result = new MatrixCursor(columns);
        String section = table(uri), name = key(uri, selection, args);
        if (section == null || name == null) return result;
        String value = get(section, name);
        if (value != null) {
            Object[] row = new Object[columns.length];
            for (int n = 0; n < columns.length; n++)
                row[n] = "name".equals(columns[n]) ? name : "value".equals(columns[n]) ? value : null;
            result.addRow(row);
        }
        return result;
    }
    @Override public Uri insert(Uri uri, ContentValues values) {
        String section = table(uri), name = values == null ? null : values.getAsString("name");
        if (section == null || name == null || !put(section, name, values.getAsString("value"))) return null;
        return Uri.withAppendedPath(uri, name);
    }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        String section = table(uri), name = key(uri, selection, args);
        return section != null && name != null && values != null && put(section, name, values.getAsString("value")) ? 1 : 0;
    }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public String getType(Uri uri) { return null; }
}
