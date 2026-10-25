// Megler: Android's download provider (content://downloads/my_downloads, the system's DownloadProvider), into which an app
// enqueues downloads by inserting a row -- what android.app.DownloadManager does, and Play Store's own old downloader, with
// which it updates itself. Each download runs on a thread of its own over HTTP (the row's headers, cookies, user agent and
// referer, redirects followed), into the file the row names (a file: hint) or one of the app's cache; the row's status and
// bytes are what a query gives, a change of them is told to the row's and the provider's URI (ContentResolver.notifyChange:
// a cursor that set its notification URI hears it), and at the end the receiver the row names is told
// android.intent.action.DOWNLOAD_COMPLETE with the download's id. openFile gives the file.
//
// Why. There was no such provider: an insert gave null, and Play Store's self-update ended at once ("null uri for
// com.android.vending", "Self-update failed because of HTTP error code: 0").
//
// Not here: a download kept across the app's runs (the rows are the app's while it runs), the network types and roaming
// a row allows (any network is taken), pausing, notifications, the public downloads folder's listing.
package megler;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.LinkedHashMap;

public final class Downloads extends ContentProvider {
    public static final Downloads INSTANCE = new Downloads();
    static final Uri CONTENT = Uri.parse("content://downloads/my_downloads");
    /* Downloads.Impl's statuses. */
    static final int PENDING = 190, RUNNING = 192, SUCCESS = 200, CANNOT_RESUME = 489, CANCELED = 490, UNKNOWN_ERROR = 491, FILE_ERROR = 492,
        UNHANDLED_HTTP_CODE = 494, HTTP_DATA_ERROR = 495, TOO_MANY_REDIRECTS = 497;
    /* Downloads.Impl's destinations: the file the row's hint names, else the app's cache. */
    static final int DESTINATION_FILE_URI = 4;
    private static final String[] COLUMNS = { "_id", "uri", "status", "current_bytes", "total_bytes", "_data", "hint", "destination", "title",
        "description", "mimetype", "visibility", "notificationpackage", "notificationclass", "allowed_network_types", "lastmod", "deleted",
        "etag", "errorMsg", "control", "otheruid", "is_public_api", "allow_roaming", "allow_metered", "is_visible_in_downloads_ui" };

    private Downloads() {}

    /** A download: what it was given, and how it is going. */
    private static final class Row {
        final long id;
        final ContentValues values;
        volatile int status = PENDING;
        volatile long current, total = -1, lastmod = System.currentTimeMillis();
        volatile String data, mimetype, error;
        volatile boolean canceled;
        Row(long id, ContentValues values) { this.id = id; this.values = values; mimetype = values.getAsString("mimetype"); }
        Uri uri() { return Uri.withAppendedPath(CONTENT, Long.toString(id)); }
    }

    private final LinkedHashMap<Long, Row> mRows = new LinkedHashMap<>();
    private long mNext = 1;

    @Override public boolean onCreate() { return true; }

    @Override public Uri insert(Uri uri, ContentValues values) {
        if (values == null || values.getAsString("uri") == null) throw new IllegalArgumentException("a download needs a uri");
        Row r;
        synchronized (mRows) { r = new Row(mNext++, new ContentValues(values)); mRows.put(r.id, r); }
        android.util.Log.i("Megler", "downloads: " + r.id + " enqueued (" + Uri.parse(r.values.getAsString("uri")).getHost() + ")");
        Thread t = new Thread(() -> run(r), "download-" + r.id);
        t.setDaemon(true);
        t.start();
        changed(r);
        return r.uri();
    }

    /* The rows a URI and a selection name: the URI's one, or those a simple selection of DownloadManager's or Play Store's
       names (by _id, by notificationclass), or all. */
    private ArrayList<Row> rows(Uri uri, String selection, String[] args) {
        ArrayList<Row> l = new ArrayList<>();
        Long only = null;
        if (uri.getPathSegments().size() > 1) try { only = Long.parseLong(uri.getLastPathSegment()); } catch (NumberFormatException e) {}
        String sel = selection == null ? "" : selection.replace(" ", "");
        java.util.HashSet<Long> ids = null;
        String cls = null;
        if (args != null && sel.contains("_id")) {
            ids = new java.util.HashSet<>();
            for (String a : args) try { ids.add(Long.parseLong(a)); } catch (NumberFormatException e) {}
            if (ids.isEmpty()) ids = null;
        }
        if (args != null && args.length > 0 && sel.startsWith("notificationclass=?")) cls = args[0];
        synchronized (mRows) {
            for (Row r : mRows.values()) {
                if (only != null && r.id != only) continue;
                if (ids != null && !ids.contains(r.id)) continue;
                if (cls != null && !cls.equals(r.values.getAsString("notificationclass"))) continue;
                l.add(r);
            }
        }
        return l;
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        String[] cols = projection != null ? projection : COLUMNS;
        MatrixCursor c = new MatrixCursor(cols);
        for (Row r : rows(uri, selection, args)) {
            Object[] row = new Object[cols.length];
            for (int i = 0; i < cols.length; i++) row[i] = value(r, cols[i]);
            c.addRow(row);
        }
        c.setNotificationUri(ContextImpl.app().getContentResolver(), uri);
        return c;
    }

    private static Object value(Row r, String col) {
        switch (col) {
        case "_id": return r.id;
        case "status": return r.status;
        case "current_bytes": return r.current;
        case "total_bytes": return r.total;
        case "_data": return r.data;
        case "mimetype": return r.mimetype;
        case "lastmod": return r.lastmod;
        case "deleted": return 0;
        case "errorMsg": return r.error;
        case "local_uri": return r.data != null ? Uri.fromFile(new File(r.data)).toString() : null;
        case "local_filename": return r.data;
        default: return r.values.get(col);
        }
    }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        ArrayList<Row> l = rows(uri, selection, args);
        for (Row r : l) {
            Integer deleted = values.getAsInteger("deleted");
            if (deleted != null && deleted != 0) remove(r);
            else { r.values.putAll(values); r.lastmod = System.currentTimeMillis(); changed(r); }
        }
        return l.size();
    }

    @Override public int delete(Uri uri, String selection, String[] args) {
        ArrayList<Row> l = rows(uri, selection, args);
        for (Row r : l) remove(r);
        return l.size();
    }

    private void remove(Row r) {
        r.canceled = true;
        synchronized (mRows) { mRows.remove(r.id); }
        Integer dest = r.values.getAsInteger("destination");
        if (r.data != null && (dest == null || dest != DESTINATION_FILE_URI)) new File(r.data).delete();
        changed(r);
    }

    @Override public String getType(Uri uri) {
        for (Row r : rows(uri, null, null)) if (r.mimetype != null) return r.mimetype;
        return "vnd.android.cursor.item/download";
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws java.io.FileNotFoundException {
        for (Row r : rows(uri, null, null))
            if (r.data != null) return ParcelFileDescriptor.open(new File(r.data), ParcelFileDescriptor.parseMode(mode));
        throw new java.io.FileNotFoundException("no downloaded file for " + uri);
    }

    /* The row and the provider's URI told of a change. */
    private static void changed(Row r) {
        android.content.ContentResolver cr = ContextImpl.app().getContentResolver();
        cr.notifyChange(r.uri(), null);
        cr.notifyChange(CONTENT, null);
    }

    private static File target(Row r) {
        Integer dest = r.values.getAsInteger("destination");
        String hint = r.values.getAsString("hint");
        if (dest != null && dest == DESTINATION_FILE_URI && hint != null) {
            Uri h = Uri.parse(hint);
            if (h.getPath() != null) return new File(h.getPath());
        }
        File dir = new File(ContextImpl.app().getCacheDir(), "megler-downloads");
        dir.mkdirs();
        return new File(dir, r.id + ".download");
    }

    private void run(Row r) {
        String url = r.values.getAsString("uri");
        int redirects = 0;
        HttpURLConnection c = null;
        try {
            for (;;) {
                if (r.canceled) return;
                c = (HttpURLConnection) new URL(url).openConnection();
                c.setInstanceFollowRedirects(false);
                c.setConnectTimeout(30000);
                c.setReadTimeout(60000);
                for (String key : r.values.keySet()) {
                    if (!key.startsWith("http_header_")) continue;
                    String h = r.values.getAsString(key);
                    int colon = h != null ? h.indexOf(':') : -1;
                    if (colon > 0) c.addRequestProperty(h.substring(0, colon).trim(), h.substring(colon + 1).trim());
                }
                if (r.values.getAsString("cookiedata") != null) c.addRequestProperty("Cookie", r.values.getAsString("cookiedata"));
                if (r.values.getAsString("useragent") != null) c.setRequestProperty("User-Agent", r.values.getAsString("useragent"));
                if (r.values.getAsString("referer") != null) c.setRequestProperty("Referer", r.values.getAsString("referer"));
                r.status = RUNNING;
                changed(r);
                int code = c.getResponseCode();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    String to = c.getHeaderField("Location");
                    c.disconnect();
                    if (to == null || ++redirects > 7) { finish(r, TOO_MANY_REDIRECTS, "redirected too many times"); return; }
                    url = new URL(new URL(url), to).toString();
                    continue;
                }
                if (code != 200) { finish(r, code >= 400 && code < 600 ? code : UNHANDLED_HTTP_CODE, "HTTP " + code); return; }
                break;
            }
            r.total = c.getContentLengthLong();
            if (r.mimetype == null && c.getContentType() != null) r.mimetype = c.getContentType().split(";")[0].trim();
            File out = target(r);
            if (out.getParentFile() != null) out.getParentFile().mkdirs();
            r.data = out.getPath();
            long told = 0, toldAt = 0;
            try (InputStream in = c.getInputStream(); FileOutputStream o = new FileOutputStream(out)) {
                byte[] chunk = new byte[1 << 16];
                for (int n; (n = in.read(chunk)) > 0; ) {
                    if (r.canceled) return;
                    o.write(chunk, 0, n);
                    r.current += n;
                    long now = android.os.SystemClock.uptimeMillis();
                    if (r.current - told >= (1 << 20) || now - toldAt >= 500) { told = r.current; toldAt = now; r.lastmod = System.currentTimeMillis(); changed(r); }
                }
            }
            if (r.total >= 0 && r.current != r.total) { finish(r, HTTP_DATA_ERROR, "ended after " + r.current + " of " + r.total + " bytes"); return; }
            if (r.total < 0) r.total = r.current;
            finish(r, SUCCESS, null);
        } catch (java.io.IOException e) {
            finish(r, r.data == null ? HTTP_DATA_ERROR : FILE_ERROR, String.valueOf(e));
        } catch (Throwable t) {
            finish(r, UNKNOWN_ERROR, String.valueOf(t));
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /* The download ended: its status told, and the receiver it names told DOWNLOAD_COMPLETE. */
    private static void finish(Row r, int status, String error) {
        if (r.canceled) return;
        r.status = status;
        r.error = error;
        r.lastmod = System.currentTimeMillis();
        android.util.Log.i("Megler", "downloads: " + r.id + " ended with " + status + ", " + r.current + " bytes" + (error != null ? " (" + error + ")" : ""));
        changed(r);
        String pkg = r.values.getAsString("notificationpackage"), cls = r.values.getAsString("notificationclass");
        Intent done = new Intent("android.intent.action.DOWNLOAD_COMPLETE").putExtra("extra_download_id", r.id);
        if (pkg != null && cls != null) done.setClassName(pkg, cls);
        else done.setPackage(pkg != null ? pkg : Native.packageName());
        try { ContextImpl.app().sendBroadcast(done); } catch (Throwable t) { android.util.Log.w("Megler", "downloads: " + r.id + "'s end could not be told: " + t); }
    }
}
