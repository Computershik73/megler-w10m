// Megler: new in the overlay. Android's media provider (content://media, the system's MediaProvider) as an app sees it: the
// photos, videos, sounds, downloads and other files of the shared storage, indexed, queried with the app's own SQL, read,
// written, saved to, thumbnailed and watched -- what MediaStore's collections are, over the folders the phone's own are
// linked into (/sdcard/DCIM is the Camera Roll, Pictures, Movies and Music the libraries; the core's mounts, megler.h's
// megler_storage_host).
//
// Why. There was none: ContentResolver answered every query of the authority "media" with an empty cursor (systemEmpty),
// so every app's own gallery was empty on every phone -- Avito's photo picker, Telegram X's attach panel, WhatsApp's
// gallery, Instagram's and VK's pickers -- while the camera itself worked; and a "save to gallery" (insert, then
// openOutputStream) got a null URI.
//
// The index. One SQLite database of the app's own (<data dir>/.megler_media.db; the app's data, not its cache, so that a
// Clear Cache keeps the ids and what the app owns): a table `files` with Android 14's columns, one row a file. An app's
// query runs against it as its SQL says -- selection and arguments, sort order, LIMIT (in the sort order, the URI's
// "limit", or the Bundle's QUERY_ARG_LIMIT and QUERY_ARG_OFFSET), GROUP BY and HAVING (QUERY_ARG_SQL_GROUP_BY, and the old
// "1) GROUP BY (bucket_id" in a selection, which the parentheses the selection is put in let through as Android 9's did) --
// with Android's filters around it: the collection's media type, what the app may see, pending and trashed items.
//
// The scan. The index is brought up to date when a query comes and the last look is more than CHECK_MS old -- no thread
// watches the storage. Each folder is listed in one call that gives every entry's size and time (Native.listFolder over
// the core's mg_path_list_stat): in the phone's own folders each File.length() and lastModified() is a call to Windows'
// broker, and a Camera Roll of thousands of photos was minutes of them. A folder whose time is what it was when it was last
// listed, and which has no folders in it, is not listed again -- its files are as they were -- unless that was more than
// RELIST_MS ago (a memory card's file system may not change a folder's time when a file is added to it). Hidden files and
// folders (a name that begins with '.') are left out, a folder with a .nomedia file and everything under it too, and
// Android/data and Android/obb, as Android's scanner leaves them; Android/media is in. The first scan of a process, and any
// that finds something or takes long, is one line of the log. What a scan finds changed is told to the collections'
// observers.
//
// Metadata. A file's size, times, MIME type (by its name) and folder come with the listing. Width, height, orientation
// (EXIF's, or a video track's matrix), the time it was taken (EXIF's DateTimeOriginal as Android's scanner reads it, a
// video's creation time; else the file's time) and duration are read from its headers (megler.MediaFiles) -- never by
// decoding pixels -- when a query asks for those columns: the rows of its result first, for up to META_BUDGET_MS, the rest
// on the provider's own thread newest first, the collections told when they are done so that the app asks again.
//
// What an app may see, as Android 14 decides it: images and videos with READ_MEDIA_IMAGES and READ_MEDIA_VIDEO, sounds
// with READ_MEDIA_AUDIO -- or, for an app whose target is Android 12L or older, READ_EXTERNAL_STORAGE (or a
// WRITE_EXTERNAL_STORAGE it holds, which implies it), which then shows the other files too; MANAGE_EXTERNAL_STORAGE shows
// everything. READ_MEDIA_VISUAL_USER_SELECTED alone (Android 14's "Allow limited access") is taken as all the images and
// videos: there is no picker here for the user to choose the few the app may see, and Megler's question is a yes or a no.
// What the app inserted itself (owner_package_name its own) it always sees, writes and deletes; the rest it may change
// only with WRITE_EXTERNAL_STORAGE or MANAGE_EXTERNAL_STORAGE, as an app with legacy storage could. megler.Permissions says
// what is granted.
//
// Saving. insert() into a collection makes the file at once, empty, under the collection's folders as Android allows them
// (Pictures or DCIM for images, Movies for videos, Music and the like for sounds, Download for downloads; another
// RELATIVE_PATH is Android's IllegalArgumentException), its name made unique as Android makes it ("name (1).jpg"), the
// extension of its MIME type added where the name has none; IS_PENDING hides it from others until update(IS_PENDING=0).
// openFile in "w" gives the file; when what was written is closed, the row is read again from the file. An insert of _data
// (an app that wrote the file itself, as before Android 10) indexes that file. update() renames and moves (DISPLAY_NAME,
// RELATIVE_PATH) and sets the flags; delete() deletes the file. Each change is told to the item's and the collections'
// observers, on the provider's thread.
//
// Thumbnails. openTypedAssetFile with ContentResolver.EXTRA_SIZE (what ContentResolver.loadThumbnail asks for), the
// thumbnails tables (MediaStore.Images.Thumbnails, Video.Thumbnails) and their items give a JPEG of the picture, decoded
// subsampled and turned upright by its EXIF orientation (android.media.ThumbnailUtils), kept in the app's cache
// (cache/megler-media) under the item's id and time, one made at a time: a Lumia 650 has 1 GB.
//
// Not here: thumbnails of videos (MediaMetadataRetriever has no frames yet) and album art; tags of sounds (title, artist,
// album: the file's name, "<unknown>" and its folder, as Android has them without tags); folders as rows of the Files
// collection; trash that renames the file (IS_TRASHED is a flag); createWriteRequest and createDeleteRequest's dialog;
// playlists and genres (empty); other apps' volumes and a memory card's own volume (a folder of one linked into /sdcard is
// indexed as part of it). The ids are this app's: another app's index numbers the same files its own way.
package megler;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.DatabaseUtils;
import android.database.MatrixCursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.concurrent.LinkedBlockingQueue;

public final class MediaProvider extends ContentProvider {
    public static final MediaProvider INSTANCE = new MediaProvider();
    private MediaProvider() {}
    @Override public boolean onCreate() { return true; }

    /** The shared storage, as MediaStore's _data paths name it. */
    public static final String ROOT = "/storage/emulated/0";
    static final String EXTERNAL = "external", PRIMARY = "external_primary", INTERNAL = "internal";
    /* How long a look at the storage stands before a query looks again; and how long a folder that seems unchanged by its
       time goes before it is listed all the same. */
    static final long CHECK_MS = 1500, RELIST_MS = 30000;
    /* How long one query may spend reading headers for its rows before it leaves the rest to the provider's thread. */
    static final long META_BUDGET_MS = 1200;
    /* The thumbnails' kinds, as MediaStore's: the picture whole within 512 x 512 (MINI), within 1024 (FULL_SCREEN), and
       cropped to 96 x 96 (MICRO). */
    public static final int MINI = 1, FULL = 2, MICRO = 3;
    private static final int SCHEMA = 1;
    private static final String TAG = "Megler";

    // ---------------------------------------------------------------------------------------------------------- columns

    /* Android 14's columns of the media tables, in one table; a collection is its media type's rows. (Its own columns,
       megler_*, are never shown: the folder a row is in, in small letters, the file's time in milliseconds as listed,
       and whether its headers were read.) */
    static final String[] COLUMNS = {
        "_id", "_data", "_display_name", "_size", "mime_type", "media_type", "title", "date_added", "date_modified", "datetaken",
        "width", "height", "resolution", "orientation", "duration", "bucket_id", "bucket_display_name", "relative_path",
        "volume_name", "is_pending", "is_trashed", "is_favorite", "is_download", "is_drm", "date_expires", "owner_package_name",
        "generation_added", "generation_modified", "format", "parent", "storage_id", "description", "mini_thumb_magic",
        "isprivate", "picasa_id", "latitude", "longitude", "document_id", "instance_id", "original_document_id", "group_id",
        "artist", "album", "album_artist", "composer", "genre", "year", "track", "album_id", "artist_id", "genre_id",
        "album_key", "artist_key", "genre_key", "title_key", "title_resource_uri", "is_music", "is_ringtone", "is_alarm",
        "is_notification", "is_podcast", "is_audiobook", "is_recording", "bookmark", "bitrate", "capture_framerate",
        "cd_track_number", "compilation", "disc_number", "num_tracks", "author", "writer", "xmp", "category", "language",
        "tags", "color_range", "color_standard", "color_transfer", "exposure_time", "f_number", "iso", "scene_capture_type",
        "bits_per_sample", "samplerate", "download_uri", "referer_uri", "inferred_date", "_modifier", "_special_format",
        "_transcode_status", "_video_codec_type", "_user_id", "redacted_uri_id", "oem_metadata" };
    private static final HashSet<String> TEXT = new HashSet<>(Arrays.asList("_data", "_display_name", "mime_type", "title",
        "resolution", "bucket_display_name", "relative_path", "volume_name", "owner_package_name", "description", "picasa_id",
        "document_id", "instance_id", "original_document_id", "artist", "album", "album_artist", "composer", "genre", "album_key",
        "artist_key", "genre_key", "title_key", "title_resource_uri", "cd_track_number", "disc_number", "author", "writer",
        "category", "language", "tags", "exposure_time", "f_number", "download_uri", "referer_uri", "_video_codec_type", "redacted_uri_id"));
    private static final HashSet<String> KNOWN = new HashSet<>(Arrays.asList(COLUMNS));
    /* What a query names when it wants what only the file's headers say. */
    private static final String[] META_COLUMNS = { "width", "height", "orientation", "datetaken", "duration", "resolution" };
    private static final String SHOWN;
    static {
        StringBuilder b = new StringBuilder();
        for (String c : COLUMNS) b.append(b.length() == 0 ? "" : ", ").append(c);
        SHOWN = b.toString();
    }

    // ----------------------------------------------------------------------------------------------------- the database

    private SQLiteDatabase mDb;
    /* Held while the index is changed: a scan, an insert, an update, a delete. */
    private final Object mScan = new Object();
    private long mLastCheck = -1;
    private boolean mScannedHere;

    private synchronized SQLiteDatabase db() {
        if (mDb != null) return mDb;
        File f = new File(Native.dataDir(), ".megler_media.db");
        try {
            mDb = open(f);
        } catch (RuntimeException e) {
            // (Whatever is wrong with it, it is an index of what is on the storage: made again.)
            Log.w(TAG, "media: the index could not be opened (" + e + "): it is made again");
            SQLiteDatabase.deleteDatabase(f);
            mDb = open(f);
        }
        return mDb;
    }

    private static SQLiteDatabase open(File f) {
        SQLiteDatabase d = SQLiteDatabase.openOrCreateDatabase(f, null);
        if (d.getVersion() != SCHEMA) {
            d.execSQL("DROP TABLE IF EXISTS files");
            d.execSQL("DROP TABLE IF EXISTS folders");
            d.execSQL("DROP TABLE IF EXISTS meta");
            StringBuilder b = new StringBuilder("CREATE TABLE files (_id INTEGER PRIMARY KEY AUTOINCREMENT, _data TEXT NOT NULL UNIQUE COLLATE NOCASE");
            for (String c : COLUMNS) {
                if (c.equals("_id") || c.equals("_data")) continue;
                String type = TEXT.contains(c) ? "TEXT" : c.equals("latitude") || c.equals("longitude") || c.equals("capture_framerate") ? "REAL"
                        : c.equals("xmp") || c.equals("oem_metadata") ? "BLOB" : "INTEGER";
                b.append(", ").append(c).append(' ').append(type);
                if (c.startsWith("is_")) b.append(" DEFAULT 0");
            }
            b.append(", megler_folder TEXT, megler_mtime INTEGER, megler_meta INTEGER DEFAULT 0)");
            d.execSQL(b.toString());
            d.execSQL("CREATE INDEX files_folder ON files (megler_folder)");
            d.execSQL("CREATE INDEX files_type ON files (media_type, date_modified)");
            d.execSQL("CREATE TABLE folders (path TEXT PRIMARY KEY, mtime INTEGER, has_dirs INTEGER, listed INTEGER, nomedia INTEGER, id INTEGER, contents TEXT, alias INTEGER)");
            d.execSQL("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)");
            d.execSQL("INSERT INTO meta VALUES ('generation', '0')");
            d.execSQL("INSERT INTO meta VALUES ('version', '" + Long.toHexString(System.currentTimeMillis()) + "')");
            d.execSQL("INSERT INTO meta VALUES ('scanned', '0')");
            d.setVersion(SCHEMA);
        }
        return d;
    }

    private String meta(String key) {
        return DatabaseUtils.stringForQuery(db(), "SELECT value FROM meta WHERE key = ?", new String[] { key });
    }
    private void setMeta(String key, String value) {
        db().execSQL("UPDATE meta SET value = ? WHERE key = ?", new Object[] { value, key });
    }
    /* MediaStore.getGeneration's counter, which every change raises: counted here, and written into the index at the end of
       each change (saveGeneration), not at each row of a scan of thousands. -1: not read yet. */
    private long mGeneration = -1;
    private synchronized long nextGeneration() {
        if (mGeneration < 0) mGeneration = Long.parseLong(meta("generation"));
        return ++mGeneration;
    }
    private synchronized void saveGeneration() {
        if (mGeneration >= 0) setMeta("generation", Long.toString(mGeneration));
    }

    /** MediaStore.getGeneration: what the last change of the index was numbered. */
    public synchronized long generation() { return mGeneration >= 0 ? mGeneration : Long.parseLong(meta("generation")); }
    /** MediaStore.getVersion: a name of this index, which changes when it is made again (its ids then mean other files). */
    public String version() { return "megler-" + meta("version"); }

    // --------------------------------------------------------------------------------------------------------- the URIs

    /** A media URI taken apart: content://media/<volume>/<collection>[/<id>]. */
    static final class Target {
        String volume, kind;
        long id = -1;
        int mediaType = -1;
        boolean ours;          /* the shared storage's volume: external, external_primary */
        String collectionPath; /* what follows the volume, without the id */
    }

    static Target target(Uri u) {
        if (u == null || !"media".equals(u.getAuthority())) return null;
        java.util.List<String> s = u.getPathSegments();
        if (s.size() < 2) return null;
        Target t = new Target();
        t.volume = s.get(0);
        t.ours = EXTERNAL.equals(t.volume) || PRIMARY.equals(t.volume);
        String a = s.get(1), b = s.size() > 2 ? s.get(2) : "";
        int idAt;
        switch (a) {
        case "images": case "video": case "audio":
            if (b.equals("media")) t.kind = a;
            else if (b.equals("thumbnails") && !a.equals("audio")) t.kind = a + "_thumbnails";
            else if (a.equals("audio") && (b.equals("albums") || b.equals("artists") || b.equals("genres") || b.equals("playlists"))) t.kind = "audio_" + b;
            else return null;
            t.mediaType = a.equals("images") ? MediaFiles.IMAGE : a.equals("video") ? MediaFiles.VIDEO : MediaFiles.AUDIO;
            t.collectionPath = a + "/" + b;
            idAt = 3;
            break;
        case "file": case "downloads":
            t.kind = a;
            t.collectionPath = a;
            idAt = 2;
            break;
        default:
            return null;
        }
        if (s.size() > idAt) {
            if (s.size() > idAt + 1) return null;
            try { t.id = Long.parseLong(s.get(idAt)); } catch (NumberFormatException e) { return null; }
        }
        return t;
    }

    private static boolean media(Target t) {
        return t.kind.equals("images") || t.kind.equals("video") || t.kind.equals("audio") || t.kind.equals("file") || t.kind.equals("downloads");
    }

    /** The item's URI in its collection: content://media/<volume>/images/media/<id> and the like, or the Files one. */
    static Uri itemUri(String volume, int mediaType, long id) {
        String c = mediaType == MediaFiles.IMAGE ? "images/media" : mediaType == MediaFiles.VIDEO ? "video/media" : mediaType == MediaFiles.AUDIO ? "audio/media" : "file";
        return Uri.parse("content://media/" + volume + "/" + c + "/" + id);
    }

    /* The rows of the collection a target names (its media type, the downloads): a clause of `files`. */
    private static String collection(Target t) {
        switch (t.kind) {
        case "images": return "media_type = 1";
        case "video": return "media_type = 3";
        case "audio": return "media_type = 2";
        case "downloads": return "is_download = 1";
        default: return "1";
        }
    }

    // ---------------------------------------------------------------------------------------------- what the app may see

    private static boolean granted(String p) { return Permissions.check("android.permission." + p) == Permissions.GRANTED; }
    /* READ_EXTERNAL_STORAGE where it still means something: an app whose target is Android 12L or older (WRITE implies it). */
    private static boolean legacyRead() {
        Manifest.read();
        boolean old = !Manifest.usesSdk || Manifest.targetSdk <= 32;
        return old && (granted("READ_EXTERNAL_STORAGE") || granted("WRITE_EXTERNAL_STORAGE")) || Permissions.asks("android.permission.MANAGE_EXTERNAL_STORAGE");
    }
    /* What lets an app change files it did not insert: legacy storage's WRITE_EXTERNAL_STORAGE, or all files' access. */
    private static boolean legacyWrite() {
        return granted("WRITE_EXTERNAL_STORAGE") || Permissions.asks("android.permission.MANAGE_EXTERNAL_STORAGE");
    }
    private static String own() { return "owner_package_name = " + DatabaseUtils.sqlEscapeString(Native.packageName()); }

    /** The rows the app may see, as a clause of `files`. */
    static String visible() {
        boolean legacy = legacyRead();
        // READ_MEDIA_VISUAL_USER_SELECTED alone is all the images and videos here: see the top.
        boolean visual = granted("READ_MEDIA_VISUAL_USER_SELECTED");
        boolean images = legacy || visual || granted("READ_MEDIA_IMAGES"), video = legacy || visual || granted("READ_MEDIA_VIDEO");
        boolean audio = legacy || granted("READ_MEDIA_AUDIO");
        if (images && video && audio && legacy) return "1";
        StringBuilder types = new StringBuilder();
        if (images) types.append("1");
        if (video) types.append(types.length() > 0 ? ", " : "").append("3");
        if (audio) types.append(types.length() > 0 ? ", " : "").append("2");
        StringBuilder b = new StringBuilder("(").append(own());
        if (types.length() > 0) b.append(" OR media_type IN (").append(types).append(')');
        if (legacy) b.append(" OR media_type NOT IN (1, 2, 3)");
        return b.append(')').toString();
    }

    // ------------------------------------------------------------------------------------------------------------ query

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sortOrder) {
        Bundle b = new Bundle();
        if (selection != null) b.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, selection);
        if (args != null) b.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, args);
        if (sortOrder != null) b.putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, sortOrder);
        return query(uri, projection, b, null);
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sortOrder, android.os.CancellationSignal c) {
        return query(uri, projection, selection, args, sortOrder);
    }

    @Override public Cursor query(Uri uri, String[] projection, Bundle queryArgs, android.os.CancellationSignal signal) {
        Bundle q = queryArgs != null ? queryArgs : Bundle.EMPTY;
        Target t = target(uri);
        if (t == null) {
            Log.w(TAG, "media: a query of " + uri + ", which is no collection here: nothing");
            return new MatrixCursor(projection != null ? projection : new String[] { "_id" });
        }
        if (!t.ours || t.kind.equals("audio_genres") || t.kind.equals("audio_playlists")) {
            // The internal volume (the system's ringtones), a volume there is not, genres and playlists: none here.
            String[] cols = projection != null ? projection : t.kind.equals("audio_genres") ? new String[] { "_id", "name" }
                    : t.kind.equals("audio_playlists") ? new String[] { "_id", "name", "_data", "date_added", "date_modified" } : COLUMNS;
            MatrixCursor empty = new MatrixCursor(cols);
            empty.setNotificationUri(resolver(), uri);
            return empty;
        }
        refresh(false);
        ArrayList<String> honored = new ArrayList<>();
        String selection = q.getString(ContentResolver.QUERY_ARG_SQL_SELECTION);
        String[] args = q.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS);
        if (selection != null) honored.add(ContentResolver.QUERY_ARG_SQL_SELECTION);
        if (args != null) honored.add(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS);
        String sort = q.getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER);
        if (sort != null) honored.add(ContentResolver.QUERY_ARG_SQL_SORT_ORDER);
        else if (q.getStringArray(ContentResolver.QUERY_ARG_SORT_COLUMNS) != null) {
            // As ContentResolver.createSqlSortClause makes it of the columns and the direction.
            sort = String.join(", ", q.getStringArray(ContentResolver.QUERY_ARG_SORT_COLUMNS));
            if (q.getInt(ContentResolver.QUERY_ARG_SORT_DIRECTION, ContentResolver.QUERY_SORT_DIRECTION_ASCENDING) == ContentResolver.QUERY_SORT_DIRECTION_DESCENDING) sort += " DESC";
            honored.add(ContentResolver.QUERY_ARG_SORT_COLUMNS);
            honored.add(ContentResolver.QUERY_ARG_SORT_DIRECTION);
        }
        String limit = q.getString(ContentResolver.QUERY_ARG_SQL_LIMIT);
        if (limit != null) honored.add(ContentResolver.QUERY_ARG_SQL_LIMIT);
        else if (q.containsKey(ContentResolver.QUERY_ARG_LIMIT)) {
            limit = Integer.toString(q.getInt(ContentResolver.QUERY_ARG_LIMIT));
            honored.add(ContentResolver.QUERY_ARG_LIMIT);
            if (q.containsKey(ContentResolver.QUERY_ARG_OFFSET)) {
                limit += " OFFSET " + q.getInt(ContentResolver.QUERY_ARG_OFFSET);
                honored.add(ContentResolver.QUERY_ARG_OFFSET);
            }
        } else if (uri.getQueryParameter("limit") != null) {
            limit = uri.getQueryParameter("limit");
        }
        String group = q.getString(ContentResolver.QUERY_ARG_SQL_GROUP_BY), having = q.getString(ContentResolver.QUERY_ARG_SQL_HAVING);
        if (group != null) honored.add(ContentResolver.QUERY_ARG_SQL_GROUP_BY);
        if (having != null) honored.add(ContentResolver.QUERY_ARG_SQL_HAVING);

        String where = filters(t, q, uri);
        String from = from(t, where);
        StringBuilder sql = new StringBuilder("SELECT ");
        sql.append(projection != null && projection.length > 0 ? String.join(", ", projection) : media(t) ? SHOWN : "*");
        sql.append(" FROM ").append(from);
        if (media(t)) {
            sql.append(" WHERE ").append(where);
            if (t.id >= 0) sql.append(" AND _id = ").append(t.id);
        } else {
            sql.append(" WHERE 1");
            if (t.id >= 0) sql.append(" AND _id = ").append(t.id);
        }
        if (selection != null && !selection.trim().isEmpty()) sql.append(" AND (").append(selection).append(')');
        if (group != null) sql.append(" GROUP BY ").append(group);
        if (having != null) sql.append(" HAVING ").append(having);
        if (sort != null && !sort.trim().isEmpty()) sql.append(" ORDER BY ").append(sort);
        if (limit != null) sql.append(" LIMIT ").append(limit);

        if (media(t) && wantsMeta(projection, selection, sort, group)) readMetaFor(where + (t.id >= 0 ? " AND _id = " + t.id : ""), selection, args);
        if (t.kind.endsWith("_thumbnails")) thumbnailsFor(from, t, selection, args);

        Cursor c = db().rawQuery(sql.toString(), args);
        c.getCount();
        c.setNotificationUri(resolver(), uri);
        if (!honored.isEmpty()) {
            Bundle extras = new Bundle();
            extras.putStringArray(ContentResolver.EXTRA_HONORED_ARGS, honored.toArray(new String[0]));
            c.setExtras(extras);
        }
        return c;
    }

    /* The clause of `files` a collection's query has around the app's own: the collection, what the app may see, pending,
       trashed and favourite items as the query asks for them (MediaStore's QUERY_ARG_MATCH_*; the URI's includePending). */
    private static String filters(Target t, Bundle q, Uri uri) {
        StringBuilder w = new StringBuilder(collection(t)).append(" AND ").append(visible());
        int pending = q.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_PENDING,
                uri.getBooleanQueryParameter("includePending", false) ? android.provider.MediaStore.MATCH_INCLUDE : android.provider.MediaStore.MATCH_DEFAULT);
        int trashed = q.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_TRASHED,
                t.id >= 0 ? android.provider.MediaStore.MATCH_INCLUDE : android.provider.MediaStore.MATCH_DEFAULT);
        int favorite = q.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_FAVORITE, android.provider.MediaStore.MATCH_DEFAULT);
        switch (pending) {
        case android.provider.MediaStore.MATCH_EXCLUDE: w.append(" AND is_pending = 0"); break;
        case android.provider.MediaStore.MATCH_ONLY: w.append(" AND is_pending = 1"); break;
        case android.provider.MediaStore.MATCH_INCLUDE: break;
        default: w.append(" AND (is_pending = 0 OR ").append(own()).append(')');
        }
        switch (trashed) {
        case android.provider.MediaStore.MATCH_ONLY: w.append(" AND is_trashed = 1"); break;
        case android.provider.MediaStore.MATCH_INCLUDE: break;
        default: w.append(" AND is_trashed = 0");
        }
        if (favorite == android.provider.MediaStore.MATCH_ONLY) w.append(" AND is_favorite = 1");
        else if (favorite == android.provider.MediaStore.MATCH_EXCLUDE) w.append(" AND is_favorite = 0");
        return w.toString();
    }

    /* What a query reads from: `files` for a collection, else a table made of its rows (albums, artists, thumbnails). */
    private String from(Target t, String where) {
        switch (t.kind) {
        case "audio_albums":
            return "(SELECT album_id AS _id, album_id, album, artist, artist_id, album_key, COUNT(*) AS numsongs, COUNT(*) AS numsongs_by_artist,"
                    + " MIN(year) AS minyear, MAX(year) AS maxyear, NULL AS album_art FROM files WHERE " + where + " GROUP BY album_id)";
        case "audio_artists":
            return "(SELECT artist_id AS _id, artist, artist_key, COUNT(DISTINCT album_id) AS number_of_albums, COUNT(*) AS number_of_tracks"
                    + " FROM files WHERE " + where + " GROUP BY artist_id)";
        case "images_thumbnails": case "video_thumbnails": {
            String idColumn = t.kind.startsWith("images") ? "image_id" : "video_id";
            String dir = DatabaseUtils.sqlEscapeString(thumbnailDir().getPath() + "/");
            StringBuilder b = new StringBuilder("(");
            int[] kinds = { MINI, MICRO };
            for (int i = 0; i < kinds.length; i++) {
                if (i > 0) b.append(" UNION ALL ");
                b.append("SELECT _id, _id AS ").append(idColumn).append(", ").append(kinds[i]).append(" AS kind, ").append(dir)
                 .append(" || _id || '-").append(kinds[i]).append("-' || megler_mtime || '.jpg' AS _data, NULL AS width, NULL AS height FROM files WHERE ")
                 .append(where);
            }
            return b.append(')').toString();
        }
        default:
            return "files";
        }
    }

    private static boolean wantsMeta(String[] projection, String selection, String sort, String group) {
        if (projection == null || projection.length == 0) return true;
        for (String p : projection) if (mentions(p)) return true;
        return mentions(selection) || mentions(sort) || mentions(group);
    }
    private static boolean mentions(String s) {
        if (s == null) return false;
        String l = s.toLowerCase(Locale.ROOT);
        for (String c : META_COLUMNS) if (l.contains(c)) return true;
        return false;
    }

    private ContentResolver resolver() { return ContextImpl.app().getContentResolver(); }

    // ----------------------------------------------------------------------------------------------------------- getType

    @Override public String getType(Uri uri) {
        Target t = target(uri);
        if (t == null) return null;
        if (t.id >= 0 && media(t)) {
            return DatabaseUtils.stringForQuery(db(), "SELECT mime_type FROM files WHERE _id = " + t.id, null);
        }
        switch (t.kind) {
        case "images": return t.id >= 0 ? "vnd.android.cursor.item/image" : "vnd.android.cursor.dir/image";
        case "video": return "vnd.android.cursor.dir/video";
        case "audio": return "vnd.android.cursor.dir/audio";
        case "audio_albums": return "vnd.android.cursor.dir/albums";
        case "audio_artists": return "vnd.android.cursor.dir/artists";
        case "images_thumbnails": case "video_thumbnails": return t.id >= 0 ? "image/jpeg" : "vnd.android.cursor.dir/image";
        default: return "vnd.android.cursor.dir/file";
        }
    }

    // ------------------------------------------------------------------------------------------------------------- rows

    /** One row of the index, as the provider's own code needs it. */
    static final class Row {
        long id, size, mtime;
        String path, mime, owner;
        int mediaType;
        boolean pending;
        boolean own() { return owner != null && owner.equals(Native.packageName()); }
    }

    private static final String ROW = "_id, _data, mime_type, media_type, owner_package_name, is_pending, _size, megler_mtime";
    private static Row row(Cursor c) {
        Row r = new Row();
        r.id = c.getLong(0);
        r.path = c.getString(1);
        r.mime = c.getString(2);
        r.mediaType = c.getInt(3);
        r.owner = c.getString(4);
        r.pending = c.getInt(5) != 0;
        r.size = c.getLong(6);
        r.mtime = c.getLong(7);
        return r;
    }

    /* The rows of a target that the app may see (pending and trashed ones included, as an item's URI has them), with the
       app's selection; only those it may change when `writing`. */
    private ArrayList<Row> rows(Target t, String selection, String[] args, boolean writing) {
        StringBuilder w = new StringBuilder("SELECT ").append(ROW).append(" FROM files WHERE ").append(collection(t)).append(" AND ").append(visible());
        if (t.id >= 0) w.append(" AND _id = ").append(t.id);
        if (writing && !legacyWrite()) w.append(" AND ").append(own());
        if (selection != null && !selection.trim().isEmpty()) w.append(" AND (").append(selection).append(')');
        ArrayList<Row> l = new ArrayList<>();
        try (Cursor c = db().rawQuery(w.toString(), args)) {
            while (c.moveToNext()) l.add(row(c));
        }
        return l;
    }

    private Row item(Uri uri) throws FileNotFoundException {
        Target t = target(uri);
        if (t == null || !t.ours || t.id < 0) throw new FileNotFoundException("No item at " + uri);
        Target base = t;
        if (t.kind.endsWith("_thumbnails")) {
            base = new Target();
            base.kind = t.kind.startsWith("images") ? "images" : "video";
            base.id = t.id;
        } else if (!media(t)) {
            throw new FileNotFoundException("No item at " + uri);
        }
        ArrayList<Row> l = rows(base, null, null, false);
        if (l.isEmpty()) throw new FileNotFoundException("No item at " + uri);
        return l.get(0);
    }

    // ---------------------------------------------------------------------------------------------------------- reading

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        Target t = target(uri);
        Row r = item(uri);
        if (t.kind.endsWith("_thumbnails")) return ParcelFileDescriptor.open(thumbnail(r, MINI), ParcelFileDescriptor.MODE_READ_ONLY);
        boolean writing = mode != null && !mode.equals("r");
        File f = new File(r.path);
        if (!writing) return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
        if (!r.own() && !legacyWrite()) throw new SecurityException(Native.packageName() + " has no access to " + uri);
        File parent = f.getParentFile();
        if (parent != null) parent.mkdirs();
        final long id = r.id;
        // When what was written is closed, the row is read again from the file (Android's provider hears the close too).
        return new ParcelFileDescriptor(ParcelFileDescriptor.open(f, ParcelFileDescriptor.parseMode(mode))) {
            private boolean closed;
            @Override public void close() throws IOException {
                try { super.close(); }
                finally {
                    boolean first;
                    synchronized (this) { first = !closed; closed = true; }
                    if (first) written(id);
                }
            }
        };
    }

    @Override public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeFilter, Bundle opts) throws FileNotFoundException {
        Object size = opts != null ? opts.get(ContentResolver.EXTRA_SIZE) : null;
        if (size != null) {
            Row r = item(uri);
            if (r.mediaType == MediaFiles.IMAGE || r.mediaType == MediaFiles.VIDEO) {
                int w = 0, h = 0;
                if (size instanceof android.graphics.Point) { w = ((android.graphics.Point) size).x; h = ((android.graphics.Point) size).y; }
                else if (size instanceof android.util.Size) { w = ((android.util.Size) size).getWidth(); h = ((android.util.Size) size).getHeight(); }
                File th = thumbnail(r, Math.max(w, h) > 512 ? FULL : MINI);
                return new AssetFileDescriptor(ParcelFileDescriptor.open(th, ParcelFileDescriptor.MODE_READ_ONLY), 0, th.length());
            }
            throw new FileNotFoundException("No thumbnail of " + uri + " (" + r.mime + ")");
        }
        Row r = item(uri);
        if (mimeFilter != null && !matches(mimeFilter, r.mime)) throw new FileNotFoundException("Can't open " + uri + " as type " + mimeFilter);
        return openAssetFile(uri, "r");
    }

    private static boolean matches(String filter, String mime) {
        if (filter.equals("*/*") || mime == null) return true;
        if (filter.endsWith("/*")) return mime.regionMatches(true, 0, filter, 0, filter.length() - 1);
        return filter.equalsIgnoreCase(mime);
    }

    // ------------------------------------------------------------------------------------------------------- thumbnails

    private static final Object sThumbnailing = new Object();

    static File thumbnailDir() {
        File d = new File(ContextImpl.app().getCacheDir(), "megler-media");
        d.mkdirs();
        return d;
    }

    /** The thumbnail of a picture or a video, made if it is not there yet, as a JPEG file of the app's cache. */
    File thumbnail(Row r, int kind) throws FileNotFoundException {
        File f = new File(thumbnailDir(), r.id + "-" + kind + "-" + r.mtime + ".jpg");
        if (f.length() > 0) return f;
        // One at a time: a photo of 13 megapixels is read whole to be decoded, and a phone has 1 GB.
        synchronized (sThumbnailing) {
            if (f.length() > 0) return f;
            android.graphics.Bitmap b;
            try {
                android.util.Size box = kind == FULL ? new android.util.Size(1024, 1024) : new android.util.Size(512, 512);
                b = r.mediaType == MediaFiles.VIDEO ? android.media.ThumbnailUtils.createVideoThumbnail(new File(r.path), box, null)
                                                   : android.media.ThumbnailUtils.createImageThumbnail(new File(r.path), box, null);
                if (b != null && kind == MICRO) b = android.media.ThumbnailUtils.extractThumbnail(b, 96, 96, android.media.ThumbnailUtils.OPTIONS_RECYCLE_INPUT);
            } catch (IOException | RuntimeException e) {
                throw new FileNotFoundException("No thumbnail of " + r.path + ": " + e);
            }
            if (b == null) throw new FileNotFoundException("No thumbnail of " + r.path);
            File part = new File(f.getPath() + ".part");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(part)) {
                if (!b.compress(android.graphics.Bitmap.CompressFormat.JPEG, 90, out)) throw new IOException("not compressed");
            } catch (IOException e) {
                part.delete();
                throw new FileNotFoundException("No thumbnail of " + r.path + ": " + e);
            } finally {
                b.recycle();
            }
            if (!part.renameTo(f)) {
                part.delete();
                throw new FileNotFoundException("No thumbnail of " + r.path);
            }
            return f;
        }
    }

    /* The thumbnails of items gone or changed. */
    private static void forgetThumbnails(java.util.Collection<Long> ids) {
        if (ids.isEmpty()) return;
        String[] names = thumbnailDir().list();
        if (names == null) return;
        HashSet<String> prefixes = new HashSet<>();
        for (long id : ids) prefixes.add(id + "-");
        for (String n : names) {
            int dash = n.indexOf('-');
            if (dash > 0 && prefixes.contains(n.substring(0, dash + 1))) new File(thumbnailDir(), n).delete();
        }
    }
    private static void forgetThumbnails(long id) { forgetThumbnails(java.util.Collections.singletonList(id)); }

    /* A query of a thumbnails table: the thumbnails of a few rows made before it (an app asks for one by its image's id,
       queryMiniThumbnail); of many, those already there are. */
    private void thumbnailsFor(String from, Target t, String selection, String[] args) {
        String idColumn = t.kind.startsWith("images") ? "image_id" : "video_id";
        StringBuilder w = new StringBuilder("SELECT ").append(idColumn).append(", kind FROM ").append(from).append(" WHERE 1");
        if (t.id >= 0) w.append(" AND _id = ").append(t.id);
        if (selection != null && !selection.trim().isEmpty()) w.append(" AND (").append(selection).append(')');
        w.append(" LIMIT 65");
        ArrayList<long[]> wanted = new ArrayList<>();
        try (Cursor c = db().rawQuery(w.toString(), args)) {
            while (c.moveToNext()) wanted.add(new long[] { c.getLong(0), c.getLong(1) });
        } catch (RuntimeException e) {
            return;
        }
        if (wanted.size() > 64) return;
        Target base = new Target();
        base.kind = t.kind.startsWith("images") ? "images" : "video";
        for (long[] k : wanted) {
            base.id = k[0];
            ArrayList<Row> l = rows(base, null, null, false);
            if (l.isEmpty()) continue;
            try { thumbnail(l.get(0), (int) k[1]); } catch (FileNotFoundException e) { }
        }
    }

    // --------------------------------------------------------------------------------------------------------- metadata

    /* Headers read for the rows a query is about to give, newest first, for up to META_BUDGET_MS; the rest on the provider's
       thread. */
    private void readMetaFor(String where, String selection, String[] args) {
        StringBuilder w = new StringBuilder("SELECT ").append(ROW).append(" FROM files WHERE megler_meta = 0 AND media_type IN (1, 2, 3) AND ").append(where);
        if (selection != null && !selection.trim().isEmpty()) w.append(" AND (").append(selection).append(')');
        w.append(" ORDER BY date_modified DESC LIMIT 400");
        ArrayList<Row> l = new ArrayList<>();
        try (Cursor c = db().rawQuery(w.toString(), args)) {
            while (c.moveToNext()) l.add(row(c));
        } catch (RuntimeException e) {
            return;   // (The app's selection is wrong: its own query will say so.)
        }
        if (l.isEmpty()) return;
        long until = SystemClock.uptimeMillis() + META_BUDGET_MS;
        int done = 0;
        for (Row r : l) {
            if (SystemClock.uptimeMillis() > until) break;
            readMeta(r);
            done++;
        }
        if (done < l.size()) fillLater();
    }

    /* A row's headers read and written into it (unless the file changed meanwhile). */
    private void readMeta(Row r) {
        MediaFiles.Meta m = MediaFiles.read(new File(r.path), r.mime, r.mtime);
        ContentValues v = new ContentValues();
        v.put("megler_meta", 1);
        if (r.mediaType == MediaFiles.IMAGE || r.mediaType == MediaFiles.VIDEO) {
            v.put("width", m.width);
            v.put("height", m.height);
            v.put("resolution", m.width > 0 && m.height > 0 ? m.width + "×" + m.height : null);
            v.put("orientation", m.orientation);
            if (m.dateTaken > 0) v.put("datetaken", m.dateTaken);
        }
        if (r.mediaType == MediaFiles.VIDEO || r.mediaType == MediaFiles.AUDIO) v.put("duration", m.duration);
        db().update("files", v, "_id = ? AND megler_mtime = ?", new String[] { Long.toString(r.id), Long.toString(r.mtime) });
    }

    private volatile boolean mFilling;

    /* The headers of every row that has not had them read, on the provider's thread, newest first; the collections told
       when it is done (and every few seconds while it goes on), so that the app asks again. */
    private void fillLater() {
        if (mFilling) return;
        mFilling = true;
        background(() -> {
            long told = SystemClock.uptimeMillis();
            HashSet<Integer> types = new HashSet<>();
            int n = 0;
            try {
                for (;;) {
                    ArrayList<Row> l = new ArrayList<>();
                    try (Cursor c = db().rawQuery("SELECT " + ROW + " FROM files WHERE megler_meta = 0 AND media_type IN (1, 2, 3) ORDER BY date_modified DESC LIMIT 32", null)) {
                        while (c.moveToNext()) l.add(row(c));
                    }
                    if (l.isEmpty()) break;
                    for (Row r : l) {
                        readMeta(r);
                        types.add(r.mediaType);
                        n++;
                    }
                    if (SystemClock.uptimeMillis() - told > 5000) {
                        told = SystemClock.uptimeMillis();
                        tellCollections(types);
                        types.clear();
                    }
                }
            } catch (RuntimeException e) {
                Log.w(TAG, "media: the files' headers could not all be read: " + e);
            } finally {
                mFilling = false;
            }
            if (!types.isEmpty()) tellCollections(types);
            if (n > 0) Log.i(TAG, "media: the headers of " + n + " files read on the provider's thread");
        });
    }

    // ------------------------------------------------------------------------------------------------------------- scan

    /** The index brought up to date with the storage, unless it was looked at less than CHECK_MS ago (`force`: all the same). */
    public void refresh(boolean force) {
        synchronized (mScan) {
            long now = SystemClock.uptimeMillis();
            if (!force && mLastCheck >= 0 && now - mLastCheck < CHECK_MS) return;
            scan();
            mLastCheck = SystemClock.uptimeMillis();
        }
    }

    /* A folder as the index last saw it: its time, whether it had folders in it, when it was listed, a .nomedia; and what
       tells it when another path leads to it: its id, or where the host says none, what is in it. */
    private static final class Folder {
        long mtime, listed, id;
        boolean hasDirs, nomedia, alias;
        String contents;
    }

    /* An entry of a listing ("." the folder itself, first; its id 0 where the host says none). */
    private static final class Entry {
        String name;
        boolean dir;
        long size, mtime, id;
    }

    /* One scan's work: what it saw and what it will change. */
    private static final class Scan {
        final HashMap<String, Folder> before = new HashMap<>(), after = new HashMap<>();
        final ArrayList<String[]> inserts = new ArrayList<>();   /* path, folder key */
        final ArrayList<long[]> inserted = new ArrayList<>();    /* size, mtime */
        final ArrayList<long[]> updates = new ArrayList<>();     /* id, size, mtime */
        final ArrayList<Long> deletes = new ArrayList<>();
        final HashSet<String> emptied = new HashSet<>();         /* folders whose rows all go (a .nomedia) */
        final HashSet<Integer> types = new HashSet<>();
        /* The folders walked, by id and by contents: a folder met again by another path is not indexed twice. */
        final HashSet<Long> ids = new HashSet<>();
        final HashSet<String> contents = new HashSet<>();
        final ArrayList<String> aliases = new ArrayList<>();
        int listed, skipped;
        long now;
    }

    private static ArrayList<Entry> list(String dir) {
        String text = null;
        try { text = Native.listFolder(dir); } catch (UnsatisfiedLinkError e) { }
        ArrayList<Entry> l = new ArrayList<>();
        if (text == null) {
            // (A core without the listing, or a folder it could not list: java.io.File's way.)
            File[] files = new File(dir).listFiles();
            if (files == null) return null;
            for (File f : files) {
                Entry e = new Entry();
                e.name = f.getName();
                e.dir = f.isDirectory();
                e.size = e.dir ? 0 : f.length();
                e.mtime = seconds(f);
                l.add(e);
            }
            return l;
        }
        for (int at = 0, n = text.length(); at < n; ) {
            int a = text.indexOf('/', at), b = a < 0 ? -1 : text.indexOf('/', a + 1), c = b < 0 ? -1 : text.indexOf('/', b + 1);
            int d = c < 0 ? -1 : text.indexOf('/', c + 1);
            if (d < 0) break;
            Entry e = new Entry();
            e.dir = text.charAt(at) == 'd';
            try {
                e.size = Long.parseLong(text.substring(at + 1, a));
                e.mtime = Long.parseLong(text.substring(a + 1, b)) / 1000 * 1000;
                e.id = Long.parseUnsignedLong(text.substring(b + 1, c));
            } catch (NumberFormatException x) { at = d + 1; continue; }
            e.name = text.substring(c + 1, d);
            l.add(e);
            at = d + 1;
        }
        return l;
    }

    private static String key(String path) { return path.toLowerCase(Locale.ROOT); }
    /* A file's time as the index keeps it, in whole seconds: what java.io.File says of a file (libcore's lastModified has
       seconds alone) and what a listing says of it must agree, or every file would look changed at every listing. */
    private static long seconds(File f) { return f.lastModified() / 1000 * 1000; }
    private static boolean hidden(String name) { return name.startsWith("."); }

    private void scan() {
        long start = SystemClock.uptimeMillis();
        SQLiteDatabase d = db();
        Scan s = new Scan();
        s.now = System.currentTimeMillis();
        try (Cursor c = d.rawQuery("SELECT path, mtime, has_dirs, listed, nomedia, id, contents, alias FROM folders", null)) {
            while (c.moveToNext()) {
                Folder f = new Folder();
                f.mtime = c.getLong(1);
                f.hasDirs = c.getInt(2) != 0;
                f.listed = c.getLong(3);
                f.nomedia = c.getInt(4) != 0;
                f.id = c.getLong(5);
                f.contents = c.getString(6);
                f.alias = c.getInt(7) != 0;
                s.before.put(c.getString(0), f);
            }
        }
        // The storage's own files, then its folders: the listing of the root itself has the files; java.io.File's has the
        // folders the core links in (the phone's own), which the root's folder on the host does not.
        ArrayList<Entry> root = list(ROOT);
        if (root == null) {
            // (Never seen: the storage's own folder is the app's data's. Nothing is taken away for it.)
            Log.w(TAG, "media: the shared storage could not be listed: the index is left as it was");
            return;
        }
        ArrayList<Entry> rootFiles = new ArrayList<>();
        HashSet<String> fileNames = new HashSet<>();
        for (Entry e : root) if (!e.dir && !e.name.equals(".")) { rootFiles.add(e); fileNames.add(e.name); }
        diff(s, ROOT, rootFiles);
        Folder rf = new Folder();
        rf.listed = s.now;
        s.after.put(key(ROOT), rf);
        String[] top = new File(ROOT).list();
        if (top != null) {
            Arrays.sort(top);
            for (String name : top) {
                if (hidden(name) || name.equalsIgnoreCase("Android") || fileNames.contains(name)) continue;
                walk(s, ROOT + "/" + name, -1);
            }
        }
        // Android/media's folders, each an app's, as Android indexes them (WhatsApp's media are there since Android 11).
        String[] apps = new File(ROOT, "Android/media").list();
        if (apps != null) for (String name : apps) if (!hidden(name)) walk(s, ROOT + "/Android/media/" + name, -1);

        int before = s.inserts.size() + s.updates.size() + s.deletes.size();
        int gone = apply(s);
        long took = SystemClock.uptimeMillis() - start;
        int changes = before + gone;
        boolean first = !mScannedHere;
        mScannedHere = true;
        if (first || changes > 0 || took > 500) {
            long items = DatabaseUtils.queryNumEntries(d, "files");
            Log.i(TAG, "media: the shared storage indexed: " + items + " files, " + s.inserts.size() + " new, " + s.updates.size() + " changed, "
                    + (s.deletes.size() + gone) + " gone; " + s.listed + " folders listed, " + s.skipped + " unchanged, in " + took + " ms");
        }
        if (!s.aliases.isEmpty() && (first || changes > 0))
            Log.i(TAG, "media: indexed once, by its first path: " + String.join(", ", s.aliases));
        if (changes > 0 && !first) tellCollections(s.types);
        if (!s.inserts.isEmpty() || !s.updates.isEmpty()) {
            boolean media = false;
            for (int t : s.types) media |= t == MediaFiles.IMAGE || t == MediaFiles.VIDEO || t == MediaFiles.AUDIO;
            if (media) fillLater();
        }
    }

    /* A folder and what is under it: listed when it may have changed, its files compared with the index's rows, its
       folders walked. `mtime` is the folder's time as its parent's listing has it (-1: not known, as for the storage's
       own folders, which are always listed). */
    private void walk(Scan s, String dir, long mtime) {
        String k = key(dir);
        Folder was = s.before.get(k);
        Boolean again = null;
        if (was != null && mtime >= 0 && was.mtime == mtime && !was.hasDirs && s.now - was.listed < RELIST_MS) {
            again = alias(s, dir, was.id, was.contents);
            // (One that was another path to a folder and is no longer, or the other way round, is listed again.)
            if (again == was.alias) {
                if (again) s.emptied.add(k);
                s.after.put(k, was);
                s.skipped++;
                return;
            }
        }
        ArrayList<Entry> l = list(dir);
        if (l == null) {
            // A folder that is there and could not be listed (a link that does not answer just now) keeps what it had.
            if (was != null && new File(dir).isDirectory()) keepBelow(s, k);
            return;
        }
        s.listed++;
        Folder f = new Folder();
        f.mtime = mtime;
        f.listed = s.now;
        ArrayList<Entry> files = new ArrayList<>();
        for (Entry e : l) {
            if (e.name.equals(".")) f.id = e.id;
            else if (!e.dir && e.name.equalsIgnoreCase(".nomedia")) f.nomedia = true;
            else if (hidden(e.name)) continue;
            else if (e.dir) f.hasDirs = true;
            else files.add(e);
        }
        if (f.id == 0) f.contents = contents(files);
        if (again == null) again = alias(s, dir, f.id, f.contents);
        if (again) {
            f.alias = true;
            f.hasDirs = false;
            s.emptied.add(k);
            s.after.put(k, f);
            return;
        }
        if (f.nomedia) {
            f.hasDirs = false;
            s.emptied.add(k);
            s.after.put(k, f);
            return;
        }
        s.after.put(k, f);
        diff(s, dir, files);
        for (Entry e : l) if (e.dir && !hidden(e.name)) walk(s, dir + "/" + e.name, e.mtime);
    }

    /*
     * Whether a folder was met already in this scan by another path -- on a phone the Camera Roll is both /sdcard/DCIM and
     * /sdcard/Pictures/Camera Roll (the shell links the pictures library, and the Camera Roll in it as DCIM): then it is
     * not indexed again, and the rows it had under this path go. Told by its id, or where the host says none (a memory
     * card's file system), by what is in it: the same names, sizes and times. The first path met is the one indexed: the
     * storage's folders are walked in the order of their names, DCIM before Pictures.
     */
    private static boolean alias(Scan s, String dir, long id, String contents) {
        boolean again = id != 0 ? !s.ids.add(id) : contents != null && !contents.isEmpty() && !s.contents.add(contents);
        if (again) s.aliases.add(dir.substring(Math.min(dir.length(), ROOT.length() + 1)));
        return again;
    }

    /* What a folder holds, its files' names, sizes and times in order, as a digest of them ("" for none). */
    private static String contents(ArrayList<Entry> files) {
        if (files.isEmpty()) return "";
        ArrayList<String> l = new ArrayList<>();
        for (Entry e : files) l.add(e.name + "/" + e.size + "/" + e.mtime);
        java.util.Collections.sort(l);
        try {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-1");
            for (String x : l) md.update((x + "|").getBytes(java.nio.charset.StandardCharsets.UTF_8));
            StringBuilder b = new StringBuilder();
            for (byte x : md.digest()) b.append(String.format("%02x", x & 255));
            return b.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            return String.join("|", l);
        }
    }

    /* The records of a folder and those under it kept as they were. */
    private static void keepBelow(Scan s, String k) {
        for (java.util.Map.Entry<String, Folder> e : s.before.entrySet())
            if (e.getKey().equals(k) || e.getKey().startsWith(k + "/")) s.after.put(e.getKey(), e.getValue());
    }

    /* A folder's files against its rows: what is new, what changed (its size or time), what is gone. */
    private void diff(Scan s, String dir, ArrayList<Entry> files) {
        HashMap<String, long[]> rows = new HashMap<>();
        HashMap<String, Integer> types = new HashMap<>();
        try (Cursor c = db().rawQuery("SELECT _id, _data, _size, megler_mtime, media_type FROM files WHERE megler_folder = ?", new String[] { key(dir) })) {
            while (c.moveToNext()) {
                String name = new File(c.getString(1)).getName();
                rows.put(key(name), new long[] { c.getLong(0), c.getLong(2), c.getLong(3) });
                types.put(key(name), c.getInt(4));
            }
        }
        for (Entry e : files) {
            long[] r = rows.remove(key(e.name));
            int type = MediaFiles.mediaTypeOf(MediaFiles.mimeOf(e.name));
            if (r == null) {
                s.inserts.add(new String[] { dir + "/" + e.name, key(dir) });
                s.inserted.add(new long[] { e.size, e.mtime });
                s.types.add(type);
            } else if (r[1] != e.size || r[2] != e.mtime) {
                s.updates.add(new long[] { r[0], e.size, e.mtime });
                s.types.add(type);
            }
        }
        for (java.util.Map.Entry<String, long[]> e : rows.entrySet()) {
            s.deletes.add(e.getValue()[0]);
            Integer t = types.get(e.getKey());
            if (t != null) s.types.add(t);
        }
    }

    /* What a scan found, into the index in one transaction: the new rows oldest first (their ids then go up with their
       time, as on a phone whose files were indexed as they came), the changed, the gone, the folders. How many rows went
       with folders that are gone or have a .nomedia now. */
    private int apply(Scan s) {
        SQLiteDatabase d = db();
        int gone = 0;
        synchronized (mScan) {
            d.beginTransaction();
            try {
                boolean scanned = "1".equals(meta("scanned"));
                Integer[] order = new Integer[s.inserts.size()];
                for (int i = 0; i < order.length; i++) order[i] = i;
                Arrays.sort(order, (a, b) -> Long.compare(s.inserted.get(a)[1], s.inserted.get(b)[1]));
                for (int i : order) {
                    String path = s.inserts.get(i)[0];
                    long[] st = s.inserted.get(i);
                    ContentValues v = describe(path, st[0], st[1], null, scanned ? s.now : -1);
                    // (A row of that path under another folder's name -- its letters' case -- takes the listing's.)
                    if (d.insertWithOnConflict("files", null, v, SQLiteDatabase.CONFLICT_IGNORE) < 0) d.update("files", v, "_data = ?", new String[] { path });
                }
                ArrayList<Long> stale = new ArrayList<>();
                for (long[] u : s.updates) {
                    ContentValues v = new ContentValues();
                    v.put("_size", u[1]);
                    v.put("date_modified", u[2] / 1000);
                    v.put("megler_mtime", u[2]);
                    v.put("megler_meta", 0);
                    v.put("generation_modified", nextGeneration());
                    d.update("files", v, "_id = " + u[0], null);
                    stale.add(u[0]);
                }
                for (long id : s.deletes) {
                    d.delete("files", "_id = " + id, null);
                    stale.add(id);
                }
                forgetThumbnails(stale);
                // The folders that are gone (not seen this time), and those with a .nomedia: their rows go with them.
                HashSet<String> drop = new HashSet<>(s.emptied);
                for (String k : s.before.keySet()) if (!s.after.containsKey(k)) drop.add(k);
                for (String k : drop) {
                    String like = k.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "/%";
                    gone += d.delete("files", "megler_folder = ? OR megler_folder LIKE ? ESCAPE '\\'", new String[] { k, like });
                }
                d.delete("folders", null, null);
                for (java.util.Map.Entry<String, Folder> e : s.after.entrySet()) {
                    ContentValues v = new ContentValues();
                    v.put("path", e.getKey());
                    v.put("mtime", e.getValue().mtime);
                    v.put("has_dirs", e.getValue().hasDirs ? 1 : 0);
                    v.put("listed", e.getValue().listed);
                    v.put("nomedia", e.getValue().nomedia ? 1 : 0);
                    v.put("id", e.getValue().id);
                    v.put("contents", e.getValue().contents);
                    v.put("alias", e.getValue().alias ? 1 : 0);
                    d.insert("folders", null, v);
                }
                if (!scanned) setMeta("scanned", "1");
                saveGeneration();
                d.setTransactionSuccessful();
            } finally {
                d.endTransaction();
            }
        }
        if (gone > 0) {
            s.types.add(MediaFiles.IMAGE);
            s.types.add(MediaFiles.VIDEO);
            s.types.add(MediaFiles.AUDIO);
        }
        return gone;
    }

    /**
     * A new row's values for a file: what its path and its listing say, as Android's provider computes them
     * (FileUtils.computeDataValues): its relative path and bucket -- the folder's path in small letters, its hashCode the
     * bucket's id --, its MIME type and media type by its name, its title its name without the extension, DATE_TAKEN the
     * file's time until its headers say otherwise. `added`: DATE_ADDED's time (-1: the file's own, for the files there
     * before the first scan, which a phone would have indexed as they came).
     */
    private ContentValues describe(String path, long size, long mtime, String owner, long added) {
        File f = new File(path);
        String name = f.getName(), parent = f.getParent();
        String mime = MediaFiles.mimeOf(name);
        int type = MediaFiles.mediaTypeOf(mime);
        String rel = relativePath(path);
        long now = System.currentTimeMillis();
        if (mtime <= 0) mtime = now;
        ContentValues v = new ContentValues();
        v.put("_data", path);
        v.put("_display_name", name);
        v.put("_size", size);
        v.put("mime_type", mime);
        v.put("media_type", type);
        int dot = name.lastIndexOf('.');
        v.put("title", dot > 0 ? name.substring(0, dot) : name);
        v.put("date_added", (added >= 0 ? added : mtime) / 1000);
        v.put("date_modified", mtime / 1000);
        if (type == MediaFiles.IMAGE || type == MediaFiles.VIDEO) v.put("datetaken", mtime);
        v.put("relative_path", rel);
        v.put("bucket_id", parent.toLowerCase(Locale.ROOT).hashCode());
        if (rel != null && !rel.equals("/")) v.put("bucket_display_name", new File(parent).getName());
        v.put("volume_name", PRIMARY);
        v.put("is_download", rel != null && rel.regionMatches(true, 0, "Download/", 0, 9) ? 1 : 0);
        v.put("storage_id", 65537);
        v.put("owner_package_name", owner);
        long g = nextGeneration();
        v.put("generation_added", g);
        v.put("generation_modified", g);
        if (type == MediaFiles.AUDIO) {
            String top = rel == null ? "" : rel.split("/")[0].toLowerCase(Locale.ROOT);
            String album = rel == null || rel.equals("/") ? "" : new File(parent).getName();
            v.put("artist", android.provider.MediaStore.UNKNOWN_STRING);
            v.put("album", album);
            v.put("album_id", parent.toLowerCase(Locale.ROOT).hashCode() & 0x7fffffffL);
            v.put("artist_id", android.provider.MediaStore.UNKNOWN_STRING.hashCode() & 0x7fffffffL);
            v.put("is_ringtone", top.equals("ringtones") ? 1 : 0);
            v.put("is_notification", top.equals("notifications") ? 1 : 0);
            v.put("is_alarm", top.equals("alarms") ? 1 : 0);
            v.put("is_podcast", top.equals("podcasts") ? 1 : 0);
            v.put("is_audiobook", top.equals("audiobooks") ? 1 : 0);
            v.put("is_recording", top.equals("recordings") ? 1 : 0);
            v.put("is_music", top.equals("ringtones") || top.equals("notifications") || top.equals("alarms") || top.equals("podcasts")
                    || top.equals("audiobooks") || top.equals("recordings") ? 0 : 1);
        }
        v.put("megler_folder", key(parent));
        v.put("megler_mtime", mtime);
        v.put("megler_meta", 0);
        return v;
    }

    /** MediaStore's RELATIVE_PATH of a path in the shared storage ("DCIM/Camera/", "/" for its own files), or null. */
    public static String relativePath(String path) {
        if (!path.regionMatches(true, 0, ROOT + "/", 0, ROOT.length() + 1)) return null;
        String rest = path.substring(ROOT.length() + 1);
        int slash = rest.lastIndexOf('/');
        return slash < 0 ? "/" : rest.substring(0, slash + 1);
    }

    /** A path of the shared storage as MediaStore's _data names it: /sdcard and its other names as /storage/emulated/0. */
    public static String canonical(String path) {
        if (path == null) return null;
        String p = new File(path).getAbsolutePath();
        try { p = new File(p).getCanonicalPath(); } catch (IOException e) { }
        String[] aliases = { "/sdcard", "/mnt/sdcard", "/storage/self/primary", "/storage/sdcard0", "/mnt/user/0/emulated/0", ROOT };
        for (String a : aliases) {
            if (p.equals(a)) return ROOT;
            if (p.startsWith(a + "/")) return ROOT + p.substring(a.length());
        }
        return p;
    }

    // ----------------------------------------------------------------------------------------------------------- insert

    private static final String[] IMAGE_DIRS = { "DCIM", "Pictures" }, VIDEO_DIRS = { "DCIM", "Movies", "Pictures" },
        AUDIO_DIRS = { "Alarms", "Audiobooks", "Music", "Notifications", "Podcasts", "Recordings", "Ringtones" },
        DOWNLOAD_DIRS = { "Download" }, OTHER_DIRS = { "Download", "Documents" };

    @Override public Uri insert(Uri uri, ContentValues values) {
        Target t = target(uri);
        if (t == null || t.id >= 0 || !media(t)) {
            Log.w(TAG, "media: an insert into " + uri + ", which takes none here");
            return null;
        }
        if (!t.ours) throw new UnsupportedOperationException("Writing to " + t.volume + " storage is not supported");
        ContentValues in = values != null ? new ContentValues(values) : new ContentValues();
        Row r;
        synchronized (mScan) {
            String data = in.getAsString("_data");
            if (data != null) r = insertPath(t, canonical(data), in);
            else r = insertNew(t, in);
            saveGeneration();
        }
        Uri item = t.kind.equals("file") || t.kind.equals("downloads") ? Uri.parse("content://media/" + t.volume + "/" + t.collectionPath + "/" + r.id)
                                                                      : itemUri(t.volume, r.mediaType, r.id);
        Log.i(TAG, "media: the app saved " + r.path.substring(Math.min(r.path.length(), ROOT.length() + 1)) + (r.pending ? ", pending," : "") + " as " + item);
        changed(r.id, r.mediaType);
        return item;
    }

    /* A new file in the collection, as Android's provider makes one of DISPLAY_NAME, MIME_TYPE and RELATIVE_PATH. */
    private Row insertNew(Target t, ContentValues in) {
        String name = in.getAsString("_display_name");
        if (name == null || name.trim().isEmpty()) name = in.getAsString("title");
        if (name == null || name.trim().isEmpty()) name = Long.toString(System.currentTimeMillis());
        name = validName(name);
        String mime = in.getAsString("mime_type");
        if (mime == null) mime = MediaFiles.mimeOf(name);
        if (mime == null) mime = t.kind.equals("images") ? "image/jpeg" : t.kind.equals("video") ? "video/mp4" : t.kind.equals("audio") ? "audio/mpeg" : "application/octet-stream";
        int type = MediaFiles.mediaTypeOf(mime);
        String family = t.kind.equals("images") ? "image/" : t.kind.equals("video") ? "video/" : t.kind.equals("audio") ? "audio/" : null;
        if (family != null && !mime.toLowerCase(Locale.ROOT).startsWith(family))
            throw new IllegalArgumentException("MIME type " + mime + " cannot be inserted into " + Uri.parse("content://media/" + t.volume + "/" + t.collectionPath)
                    + "; expected MIME type under " + family + "*");
        // The extension of the MIME type, where the name has none or one of another type (Android adds it the same way).
        String ext = MediaFiles.extensionOf(mime), byName = MediaFiles.mimeOf(name);
        if (ext != null && (byName == null || !sameType(byName, mime))) name = name + "." + ext;
        String[] allowed = t.kind.equals("images") ? IMAGE_DIRS : t.kind.equals("video") ? VIDEO_DIRS : t.kind.equals("audio") ? AUDIO_DIRS
                : t.kind.equals("downloads") ? DOWNLOAD_DIRS : type == MediaFiles.IMAGE ? IMAGE_DIRS : type == MediaFiles.VIDEO ? VIDEO_DIRS
                : type == MediaFiles.AUDIO ? AUDIO_DIRS : OTHER_DIRS;
        String rel = in.getAsString("relative_path");
        if (rel == null || rel.trim().isEmpty() || rel.trim().equals("/")) {
            rel = t.kind.equals("downloads") ? "Download" : type == MediaFiles.IMAGE ? "Pictures" : type == MediaFiles.VIDEO ? "Movies"
                    : type == MediaFiles.AUDIO ? "Music" : "Download";
        }
        ArrayList<String> parts = new ArrayList<>();
        for (String p : rel.split("/")) {
            p = p.trim();
            if (p.isEmpty() || p.equals(".")) continue;
            if (p.equals("..")) throw new IllegalArgumentException("Invalid path " + rel);
            parts.add(validName(p));
        }
        if (parts.isEmpty()) parts.add(allowed[0]);
        String topDir = null;
        for (String a : allowed) if (a.equalsIgnoreCase(parts.get(0))) topDir = a;
        if (topDir == null)
            throw new IllegalArgumentException("Primary directory " + parts.get(0) + " not allowed for " + Uri.parse("content://media/" + t.volume + "/" + t.collectionPath)
                    + "; allowed directories are " + Arrays.toString(allowed));
        parts.set(0, topDir);
        File dir = new File(ROOT, String.join("/", parts));
        dir.mkdirs();
        File f = unique(dir, name);
        try {
            if (!f.createNewFile()) throw new IllegalStateException("Failed to create " + f);
        } catch (IOException e) {
            throw new IllegalStateException("Failed to create " + f + ": " + e);
        }
        boolean pending = in.getAsInteger("is_pending") != null && in.getAsInteger("is_pending") != 0;
        long now = System.currentTimeMillis();
        ContentValues v = describe(f.getPath(), 0, seconds(f), Native.packageName(), now);
        v.put("mime_type", mime);
        v.put("media_type", type);
        v.put("date_added", now / 1000);
        v.put("is_pending", pending ? 1 : 0);
        if (pending) v.put("date_expires", now / 1000 + 7 * 86400);
        if (type == MediaFiles.IMAGE || type == MediaFiles.VIDEO) v.put("datetaken", now);
        copyAllowed(in, v);
        long id = db().insert("files", null, v);
        if (id < 0) throw new IllegalStateException("Failed to insert " + f);
        Row r = new Row();
        r.id = id;
        r.path = f.getPath();
        r.mime = mime;
        r.mediaType = type;
        r.owner = Native.packageName();
        r.pending = pending;
        return r;
    }

    /* An insert that names its file (_data), as an app did before Android 10 for a file it wrote itself: that file indexed,
       the app its owner; one not there yet made, empty, for the app to write. */
    private Row insertPath(Target t, String path, ContentValues in) {
        File f = new File(path);
        if (!f.exists()) {
            File parent = f.getParentFile();
            if (parent != null) parent.mkdirs();
            try { f.createNewFile(); } catch (IOException e) { throw new IllegalStateException("Failed to create " + f + ": " + e); }
        }
        long id = DatabaseUtils.longForQuery(db(), "SELECT COALESCE(MAX(_id), -1) FROM files WHERE _data = ?", new String[] { path });
        ContentValues v;
        if (id < 0) {
            v = describe(f.getPath(), f.length(), seconds(f), Native.packageName(), System.currentTimeMillis());
            if (in.getAsString("mime_type") != null) {
                v.put("mime_type", in.getAsString("mime_type"));
                v.put("media_type", MediaFiles.mediaTypeOf(in.getAsString("mime_type")));
            }
            copyAllowed(in, v);
            id = db().insert("files", null, v);
            if (id < 0) throw new IllegalStateException("Failed to insert " + f);
        } else {
            v = new ContentValues();
            v.put("owner_package_name", Native.packageName());
            copyAllowed(in, v);
            v.put("generation_modified", nextGeneration());
            db().update("files", v, "_id = " + id, null);
        }
        Row r = rowById(id);
        readMeta(r);
        return r;
    }

    private Row rowById(long id) {
        try (Cursor c = db().rawQuery("SELECT " + ROW + " FROM files WHERE _id = " + id, null)) {
            return c.moveToFirst() ? row(c) : null;
        }
    }

    /* What an app may set of a row itself: the descriptive columns, not the file's own facts nor the index's bookkeeping. */
    private static final HashSet<String> SETTABLE = new HashSet<>(Arrays.asList("title", "description", "datetaken", "orientation",
        "is_favorite", "is_trashed", "date_expires", "artist", "album", "album_artist", "composer", "genre", "year", "track",
        "is_music", "is_ringtone", "is_alarm", "is_notification", "is_podcast", "is_audiobook", "is_recording", "bookmark",
        "download_uri", "referer_uri", "latitude", "longitude", "isprivate", "tags", "category", "language", "author", "writer",
        "width", "height", "duration", "document_id", "instance_id", "original_document_id"));
    private static void copyAllowed(ContentValues in, ContentValues out) {
        for (String k : in.keySet()) if (SETTABLE.contains(k)) putValue(out, k, in.get(k));
    }
    private static void putValue(ContentValues v, String k, Object o) {
        if (o == null) v.putNull(k);
        else if (o instanceof Integer) v.put(k, (Integer) o);
        else if (o instanceof Long) v.put(k, (Long) o);
        else if (o instanceof Boolean) v.put(k, ((Boolean) o) ? 1 : 0);
        else if (o instanceof Double) v.put(k, (Double) o);
        else if (o instanceof Float) v.put(k, (Float) o);
        else if (o instanceof byte[]) v.put(k, (byte[]) o);
        else v.put(k, o.toString());
    }

    private static boolean sameType(String a, String b) {
        a = a.toLowerCase(Locale.ROOT);
        b = b.toLowerCase(Locale.ROOT);
        return a.equals(b) || (a.endsWith("bmp") && b.endsWith("bmp")) || (a.endsWith("wav") && b.endsWith("wav"));
    }

    /* A name Windows and FAT take, as Android's FileUtils.buildValidFatFilename makes it: each of " * / : < > ? \ | and the
       characters below 32 an underscore. And one that would be hidden (a dot first) has an underscore before it: the
       scanner leaves hidden files out, and the row would go with the next scan. */
    static String validName(String name) {
        StringBuilder b = new StringBuilder(name.length());
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            b.append(c < 32 || c == 127 || "\"*/:<>?\\|".indexOf(c) >= 0 ? '_' : c);
        }
        String s = b.toString().trim();
        if (s.isEmpty()) s = "_";
        if (s.startsWith(".")) s = "_" + s;
        return s;
    }

    /* A name not taken in the folder, as Android's FileUtils.buildUniqueFile makes it: "name (1).ext", "name (2).ext" ... */
    private File unique(File dir, String name) {
        File f = new File(dir, name);
        if (!taken(f)) return f;
        int dot = name.lastIndexOf('.');
        String base = dot > 0 ? name.substring(0, dot) : name, ext = dot > 0 ? name.substring(dot) : "";
        for (int n = 1; n < 10000; n++) {
            f = new File(dir, base + " (" + n + ")" + ext);
            if (!taken(f)) return f;
        }
        throw new IllegalStateException("Failed to build unique file: " + new File(dir, name));
    }
    private boolean taken(File f) {
        return f.exists() || DatabaseUtils.longForQuery(db(), "SELECT COUNT(*) FROM files WHERE _data = ?", new String[] { f.getPath() }) > 0;
    }

    // ------------------------------------------------------------------------------------------------- update and delete

    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        Target t = target(uri);
        if (t == null || !t.ours || !media(t) || values == null) return 0;
        int n = 0;
        synchronized (mScan) {
            for (Row r : rows(t, selection, args, true)) {
                if (updateRow(r, values)) n++;
            }
            saveGeneration();
        }
        return n;
    }
    @Override public int update(Uri uri, ContentValues values, Bundle extras) {
        return update(uri, values, extras != null ? extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION) : null,
                extras != null ? extras.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS) : null);
    }

    private boolean updateRow(Row r, ContentValues values) {
        ContentValues v = new ContentValues();
        copyAllowed(values, v);
        File f = new File(r.path);
        // A new name or folder moves the file, as Android's provider does for DISPLAY_NAME and RELATIVE_PATH.
        String name = values.getAsString("_display_name"), rel = values.getAsString("relative_path");
        if (name != null || rel != null) {
            File dir = f.getParentFile();
            if (rel != null) {
                String r2 = rel.replaceAll("^/+", "");
                dir = new File(ROOT, r2);
            }
            File to = new File(dir, name != null ? validName(name) : f.getName());
            if (!to.getPath().equalsIgnoreCase(f.getPath())) {
                dir.mkdirs();
                to = unique(dir, to.getName());
                if (!f.renameTo(to) && !copyThenDelete(f, to)) {
                    Log.w(TAG, "media: " + r.path + " could not be moved to " + to);
                    return false;
                }
                ContentValues d = describe(to.getPath(), to.length(), seconds(to), r.owner, -1);
                for (String k : new String[] { "_data", "_display_name", "title", "relative_path", "bucket_id", "bucket_display_name", "is_download",
                                               "megler_folder", "mime_type", "media_type" })
                    putValue(v, k, d.get(k));
                f = to;
            }
        }
        Integer pending = values.getAsInteger("is_pending");
        if (pending != null) {
            v.put("is_pending", pending != 0 ? 1 : 0);
            if (pending == 0) {
                v.putNull("date_expires");
                v.put("_size", f.length());
                v.put("date_modified", seconds(f) / 1000);
                v.put("megler_mtime", seconds(f));
                v.put("megler_meta", 0);
            }
        }
        if (values.containsKey("_data") && values.getAsString("_data") != null && !v.containsKey("_data")) {
            File to = new File(canonical(values.getAsString("_data")));
            if (!to.getPath().equalsIgnoreCase(f.getPath()) && (to.exists() || f.renameTo(to))) {
                ContentValues d = describe(to.getPath(), to.length(), seconds(to), r.owner, -1);
                for (String k : new String[] { "_data", "_display_name", "title", "relative_path", "bucket_id", "bucket_display_name", "is_download", "megler_folder" })
                    putValue(v, k, d.get(k));
            }
        }
        if (v.size() == 0) return true;
        v.put("generation_modified", nextGeneration());
        if (!v.containsKey("date_modified") && (v.containsKey("_data") || v.containsKey("is_pending"))) v.put("date_modified", System.currentTimeMillis() / 1000);
        db().update("files", v, "_id = " + r.id, null);
        if (Integer.valueOf(0).equals(pending) || v.containsKey("_data")) {
            forgetThumbnails(r.id);
            Row now = rowById(r.id);
            if (now != null) readMeta(now);
        }
        changed(r.id, r.mediaType);
        return true;
    }

    private static boolean copyThenDelete(File from, File to) {
        try (java.io.FileInputStream in = new java.io.FileInputStream(from); java.io.FileOutputStream out = new java.io.FileOutputStream(to)) {
            byte[] b = new byte[65536];
            for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        } catch (IOException e) {
            to.delete();
            return false;
        }
        from.delete();
        return true;
    }

    @Override public int delete(Uri uri, String selection, String[] args) {
        Target t = target(uri);
        if (t == null || !t.ours || !media(t)) return 0;
        int n = 0;
        synchronized (mScan) {
            for (Row r : rows(t, selection, args, true)) {
                File f = new File(r.path);
                if (f.exists() && !f.delete()) {
                    Log.w(TAG, "media: " + r.path + " could not be deleted");
                    continue;
                }
                db().delete("files", "_id = " + r.id, null);
                nextGeneration();
                forgetThumbnails(r.id);
                changed(r.id, r.mediaType);
                n++;
            }
            saveGeneration();
        }
        return n;
    }
    @Override public int delete(Uri uri, Bundle extras) {
        return delete(uri, extras != null ? extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION) : null,
                extras != null ? extras.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS) : null);
    }

    /* What was written through openFile closed: the row read again from its file. */
    private void written(long id) {
        File f;
        Row r = rowById(id);
        if (r == null) return;
        f = new File(r.path);
        ContentValues v = new ContentValues();
        v.put("_size", f.length());
        v.put("date_modified", seconds(f) / 1000);
        v.put("megler_mtime", seconds(f));
        v.put("megler_meta", 0);
        v.put("generation_modified", nextGeneration());
        db().update("files", v, "_id = " + id, null);
        saveGeneration();
        forgetThumbnails(id);
        Row now = rowById(id);
        if (now != null) readMeta(now);
        changed(id, r.mediaType);
    }

    // ------------------------------------------------------------------------------------------------------- the scanner

    /**
     * One file indexed now, as MediaScannerConnection.scanFile and MediaStore.scanFile ask (and the old broadcast
     * ACTION_MEDIA_SCANNER_SCAN_FILE): its item's URI, or null for a file that is not there, is hidden, or is not in the
     * shared storage. A folder is scanned with what is under it (null).
     */
    public Uri scanPath(String path) {
        String p = canonical(path);
        if (p == null || !p.regionMatches(true, 0, ROOT + "/", 0, ROOT.length() + 1)) return null;
        File f = new File(p);
        if (f.isDirectory()) {
            refresh(true);
            return null;
        }
        String rel = relativePath(p);
        for (String part : (rel + f.getName()).split("/")) if (hidden(part)) return null;
        if (rel.regionMatches(true, 0, "Android/data/", 0, 13) || rel.regionMatches(true, 0, "Android/obb/", 0, 12)) return null;
        for (File d = f.getParentFile(); d != null && d.getPath().length() > ROOT.length(); d = d.getParentFile())
            if (new File(d, ".nomedia").exists()) return null;
        Row r;
        synchronized (mScan) {
            long id = DatabaseUtils.longForQuery(db(), "SELECT COALESCE(MAX(_id), -1) FROM files WHERE _data = ?", new String[] { p });
            if (!f.isFile()) {
                if (id >= 0) {
                    Row gone = rowById(id);
                    db().delete("files", "_id = " + id, null);
                    forgetThumbnails(id);
                    if (gone != null) changed(id, gone.mediaType);
                }
                return null;
            }
            if (id < 0) {
                id = db().insert("files", null, describe(p, f.length(), seconds(f), null, System.currentTimeMillis()));
            } else {
                ContentValues v = new ContentValues();
                v.put("_size", f.length());
                v.put("date_modified", seconds(f) / 1000);
                v.put("megler_mtime", seconds(f));
                v.put("megler_meta", 0);
                v.put("generation_modified", nextGeneration());
                db().update("files", v, "_id = " + id, null);
                forgetThumbnails(id);
            }
            saveGeneration();
            r = rowById(id);
        }
        if (r == null) return null;
        readMeta(r);
        changed(r.id, r.mediaType);
        return itemUri(EXTERNAL, r.mediaType, r.id);
    }

    /** scanPath on the provider's thread, the answer to `done` there. */
    public static void scanLater(final String[] paths, final java.util.function.BiConsumer<String, Uri> done) {
        background(() -> {
            for (String p : paths) {
                Uri u = null;
                try { u = INSTANCE.scanPath(p); } catch (RuntimeException e) { Log.w(TAG, "media: " + p + " could not be scanned: " + e); }
                if (done != null) {
                    try { done.accept(p, u); } catch (RuntimeException e) { Log.w(TAG, "media: a scan's listener threw " + e); }
                }
            }
        });
    }

    /** The old way to have a file indexed, a broadcast (Intent.ACTION_MEDIA_SCANNER_SCAN_FILE with its file: URI): scanned. */
    public static void broadcast(android.content.Intent i) {
        if (i == null || !"android.intent.action.MEDIA_SCANNER_SCAN_FILE".equals(i.getAction())) return;
        Uri u = i.getData();
        if (u == null || u.getPath() == null || !(u.getScheme() == null || "file".equals(u.getScheme()))) return;
        scanLater(new String[] { u.getPath() }, null);
    }

    // ------------------------------------------------------------------------------------------------------------- call

    @Override public Bundle call(String method, String arg, Bundle extras) {
        Bundle out = new Bundle();
        switch (method == null ? "" : method) {
        case android.provider.MediaStore.GET_VERSION_CALL: out.putString("android.intent.extra.TEXT", version()); return out;
        case android.provider.MediaStore.GET_GENERATION_CALL: out.putLong("android.intent.extra.INDEX", generation()); return out;
        case android.provider.MediaStore.SCAN_VOLUME_CALL: refresh(true); return out;
        case android.provider.MediaStore.SCAN_FILE_CALL: {
            String p = arg != null ? arg : extras != null ? extras.getString("android.intent.extra.STREAM") : null;
            Uri u = p != null ? scanPath(p) : null;
            if (u != null) out.putParcelable("android.intent.extra.STREAM", u);
            return out;
        }
        default:
            return null;
        }
    }

    // ---------------------------------------------------------------------------------------------- the provider's thread

    private static final LinkedBlockingQueue<Runnable> sWork = new LinkedBlockingQueue<>();
    private static Thread sThread;

    /** Work on the provider's own thread, after what is there before it (MediaScannerConnection's onMediaScannerConnected). */
    public static void later(Runnable r) { background(r); }

    /** Work on the provider's own thread, one thing after another: telling observers, reading headers, scanning a file. */
    static synchronized void background(Runnable r) {
        sWork.add(r);
        if (sThread != null) return;
        sThread = new Thread(() -> {
            for (;;) {
                try { sWork.take().run(); }
                catch (InterruptedException e) { return; }
                catch (Throwable t) { Log.w(TAG, "media: the provider's thread: " + t); }
            }
        }, "megler-media");
        sThread.setDaemon(true);
        sThread.start();
    }

    // --------------------------------------------------------------------------------------------------------- telling

    /* An item changed (made, changed, deleted): its URIs and its collections' told, as Android's provider tells them -- the
       item under both names of the volume, in its collection and in Files -- and the collections themselves, which an
       observer that does not take descendants hears (Android 9's provider told those). */
    private void changed(long id, int mediaType) {
        LinkedHashSet<Uri> uris = new LinkedHashSet<>();
        for (String volume : new String[] { EXTERNAL, PRIMARY }) {
            uris.add(itemUri(volume, mediaType, id));
            uris.add(Uri.parse("content://media/" + volume + "/file/" + id));
        }
        HashSet<Integer> types = new HashSet<>();
        types.add(mediaType);
        uris.addAll(collections(types));
        tell(uris);
    }

    private static LinkedHashSet<Uri> collections(java.util.Collection<Integer> types) {
        LinkedHashSet<Uri> uris = new LinkedHashSet<>();
        for (String volume : new String[] { EXTERNAL, PRIMARY }) {
            for (int t : types) {
                if (t == MediaFiles.IMAGE) uris.add(Uri.parse("content://media/" + volume + "/images/media"));
                else if (t == MediaFiles.VIDEO) uris.add(Uri.parse("content://media/" + volume + "/video/media"));
                else if (t == MediaFiles.AUDIO) uris.add(Uri.parse("content://media/" + volume + "/audio/media"));
            }
            uris.add(Uri.parse("content://media/" + volume + "/file"));
        }
        return uris;
    }

    private void tellCollections(java.util.Collection<Integer> types) { tell(collections(types)); }

    private void tell(final java.util.Collection<Uri> uris) {
        final ContentResolver cr = resolver();
        background(() -> { for (Uri u : uris) cr.notifyChange(u, null); });
    }
}
