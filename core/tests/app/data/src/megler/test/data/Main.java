package megler.test.data;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.DatabaseUtils;
import android.database.sqlite.SQLiteConstraintException;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;
import android.database.sqlite.SQLiteOpenHelper;
import android.database.sqlite.SQLiteStatement;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.util.Arrays;

/**
 * A database used the way an app uses one: a SQLiteOpenHelper, ContentValues, queries with arguments, Cursors, transactions.
 * Each step is a line of the log, "name ok" or "name FAILED" and why; what each expects is what Android's SQLite gives. The
 * database is left with three rows at the end, and a count of how often the app has run: the next run must find them.
 */
public class Main extends Activity {
    private final StringBuilder lines = new StringBuilder();
    private int ok, failed;
    private TextView text;
    private String detail;

    private interface Step {
        boolean run() throws Throwable;
    }

    private void step(String name, Step s) {
        boolean held;
        detail = null;
        try {
            held = s.run();
        } catch (Throwable t) {
            held = false;
            detail = t.toString();
        }
        if (held) ok++;
        else failed++;
        String line = name + (held ? " ok" : " FAILED" + (detail == null ? "" : ": " + detail));
        Log.i("DataTest", line);
        lines.append(line).append('\n');
        text.setText(lines);
    }

    static class Notes extends SQLiteOpenHelper {
        int created, upgraded;

        Notes(Context c, String name, int version) {
            super(c, name, null, version);
        }

        @Override
        public void onCreate(SQLiteDatabase db) {
            db.execSQL("CREATE TABLE notes (_id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL UNIQUE, body TEXT, stars INTEGER, weight REAL, data BLOB)");
            db.execSQL("CREATE TABLE runs (n INTEGER)");
            created++;
        }

        @Override
        public void onUpgrade(SQLiteDatabase db, int from, int to) {
            db.execSQL("ALTER TABLE notes ADD COLUMN extra TEXT DEFAULT 'added'");
            upgraded++;
        }
    }

    private static ContentValues note(String title, String body, Integer stars, Double weight, byte[] data) {
        ContentValues v = new ContentValues();
        v.put("title", title);
        if (body == null) v.putNull("body");
        else v.put("body", body);
        v.put("stars", stars);
        v.put("weight", weight);
        v.put("data", data);
        return v;
    }

    private static long count(SQLiteDatabase db) {
        return DatabaseUtils.queryNumEntries(db, "notes");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setTextSize(10);
        /* Above the text, a strip two pixels high for a popup to hang from (popup(), at the end). */
        android.widget.LinearLayout column = new android.widget.LinearLayout(this);
        column.setOrientation(android.widget.LinearLayout.VERTICAL);
        anchor = new android.view.View(this);
        column.addView(anchor, new android.widget.LinearLayout.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT, 2));
        column.addView(text);
        /* The frame the window's insets are relative to (WindowInsets.getFrame, API 36, by reflection): the window's size. */
        column.setOnApplyWindowInsetsListener((v, insets) -> {
            Object frame;
            try {
                frame = insets.getClass().getMethod("getFrame").invoke(insets);
            } catch (Exception e) {
                frame = e;
            }
            if (!insetsLogged) Log.i("DataTest", "insets-frame " + frame);
            insetsLogged = true;
            return v.onApplyWindowInsets(insets);
        });
        setContentView(column);
        Log.i("DataTest", "onCreate");
        final Notes helper = new Notes(this, "notes.db", 1);
        final SQLiteDatabase db = helper.getWritableDatabase();
        final long run = DatabaseUtils.longForQuery(db, "SELECT count(*) FROM runs", null) + 1;
        Log.i("DataTest", "run " + run);

        step("open", () -> {
            File file = getDatabasePath("notes.db");
            detail = "created=" + helper.created + " version=" + db.getVersion() + " file=" + file + " " + file.length() + " path=" + db.getPath();
            /* Made by onCreate on the first run and not again; the file is where Android keeps an app's databases. */
            return helper.created == (run == 1 ? 1 : 0) && db.isOpen() && db.getVersion() == 1 && file.exists() && file.length() >= 1024
                    && file.getPath().equals(db.getPath()) && file.getPath().endsWith("/databases/notes.db") && !db.isReadOnly();
        });
        step("kept", () -> {
            /* What the run before left: nothing before the first, three rows after any other. */
            long n = count(db);
            String first = n == 0 ? null : DatabaseUtils.stringForQuery(db, "SELECT title || '/' || body FROM notes ORDER BY _id LIMIT 1", null);
            detail = "rows=" + n + " first=" + first;
            return run == 1 ? n == 0 : n == 3 && ("kept-1/left by run " + (run - 1)).equals(first);
        });
        db.delete("notes", null, null);
        final long[] ids = new long[3];
        step("insert", () -> {
            ids[0] = db.insert("notes", null, note("alpha", null, 1, 0.5, new byte[] { 1, 2, 3, 0, -1 }));
            ids[1] = db.insert("notes", null, note("beta", "second", 3, 2.25, null));
            ids[2] = db.insertOrThrow("notes", null, note("gamma", "third", 4, -1e10, new byte[0]));
            detail = Arrays.toString(ids) + " rows=" + count(db);
            /* AUTOINCREMENT: ids go up by one and are never used again, also across runs. */
            return ids[0] >= 1 + 5 * (run - 1) && ids[1] == ids[0] + 1 && ids[2] == ids[1] + 1 && count(db) == 3;
        });
        step("query", () -> {
            Cursor c = db.query("notes", new String[] { "_id", "title", "stars" }, "stars >= ? AND title <> ?", new String[] { "3", "nothing" }, null, null, "title DESC");
            try {
                boolean held = c.getCount() == 2 && c.getColumnCount() == 3 && c.getColumnIndex("stars") == 2 && c.getColumnIndex("nope") == -1 && "title".equals(c.getColumnName(1));
                held &= c.moveToFirst() && "gamma".equals(c.getString(1)) && c.getInt(2) == 4 && c.getLong(0) == ids[2] && c.isFirst();
                held &= c.moveToNext() && "beta".equals(c.getString(1)) && c.getInt(2) == 3 && c.isLast();
                held &= !c.moveToNext() && c.isAfterLast() && c.moveToPosition(0) && "gamma".equals(c.getString(c.getColumnIndexOrThrow("title")));
                return held;
            } finally {
                c.close();
            }
        });
        step("types", () -> {
            Cursor c = db.rawQuery("SELECT title, stars, weight, data, body, stars * 2, weight / 4 FROM notes WHERE title = ?", new String[] { "alpha" });
            try {
                if (!c.moveToFirst()) return false;
                detail = c.getType(0) + " " + c.getType(1) + " " + c.getType(2) + " " + c.getType(3) + " " + c.getType(4) + " " + Arrays.toString(c.getBlob(3));
                return c.getType(0) == Cursor.FIELD_TYPE_STRING && c.getType(1) == Cursor.FIELD_TYPE_INTEGER && c.getType(2) == Cursor.FIELD_TYPE_FLOAT
                        && c.getType(3) == Cursor.FIELD_TYPE_BLOB && c.getType(4) == Cursor.FIELD_TYPE_NULL && c.isNull(4) && c.getString(4) == null
                        && Arrays.equals(c.getBlob(3), new byte[] { 1, 2, 3, 0, -1 }) && c.getDouble(2) == 0.5 && c.getLong(5) == 2 && c.getDouble(6) == 0.125
                        && "1".equals(c.getString(1)) && c.getInt(2) == 0 && c.getCount() == 1;
            } finally {
                c.close();
            }
        });
        step("update", () -> {
            ContentValues v = new ContentValues();
            v.put("stars", 5);
            v.put("body", "changed");
            int n = db.update("notes", v, "title = ?", new String[] { "beta" });
            int none = db.update("notes", v, "title = ?", new String[] { "nobody" });
            String now = DatabaseUtils.stringForQuery(db, "SELECT body || stars FROM notes WHERE _id = ?", new String[] { String.valueOf(ids[1]) });
            detail = n + " " + none + " " + now;
            return n == 1 && none == 0 && "changed5".equals(now);
        });
        step("delete", () -> db.delete("notes", "stars < ?", new String[] { "2" }) == 1 && count(db) == 2 && db.delete("notes", "stars > 100", null) == 0);
        step("rollback", () -> {
            db.beginTransaction();
            boolean inside;
            try {
                db.insertOrThrow("notes", null, note("never", "rolled back", 9, 9.0, null));
                inside = db.inTransaction() && count(db) == 3;
            } finally {
                db.endTransaction();
            }
            detail = "inside=" + inside + " rows=" + count(db);
            return inside && !db.inTransaction() && count(db) == 2;
        });
        step("commit", () -> {
            db.beginTransaction();
            try {
                db.insertOrThrow("notes", null, note("delta", "committed", 2, 1.0, null));
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
            return count(db) == 3 && DatabaseUtils.longForQuery(db, "SELECT stars FROM notes WHERE title = 'delta'", null) == 2;
        });
        step("constraint", () -> {
            boolean thrown = false;
            try {
                db.insertOrThrow("notes", null, note("delta", "again", 0, 0.0, null));
            } catch (SQLiteConstraintException e) {
                thrown = true;
                detail = e.getMessage();
            }
            /* insert() does not throw: it answers -1. And a column that may not be null. */
            long quiet = db.insert("notes", null, note("delta", "again", 0, 0.0, null));
            ContentValues untitled = new ContentValues();
            untitled.put("body", "no title");
            return thrown && quiet == -1 && db.insert("notes", null, untitled) == -1 && count(db) == 3;
        });
        step("statement", () -> {
            SQLiteStatement s = db.compileStatement("SELECT count(*) FROM notes WHERE stars > ?");
            s.bindLong(1, 3);
            long more = s.simpleQueryForLong();
            s.bindLong(1, 0);
            long all = s.simpleQueryForLong();
            s.close();
            SQLiteStatement u = db.compileStatement("UPDATE notes SET weight = weight + ? WHERE stars > ?");
            u.bindDouble(1, 0.5);
            u.bindLong(2, 3);
            int changed = u.executeUpdateDelete();
            u.close();
            detail = more + " " + all + " " + changed;
            return more == 2 && all == 3 && changed == 2;
        });
        step("text", () -> {
            /* Text beyond ASCII goes in and comes out as it was; LOCALIZED is a collation every Android database has. */
            db.insertOrThrow("notes", null, note("Ёж € 😀", "unicode", 0, 0.0, null));
            String back = DatabaseUtils.stringForQuery(db, "SELECT title FROM notes WHERE body = 'unicode'", null);
            long length = DatabaseUtils.longForQuery(db, "SELECT length(title) FROM notes WHERE body = 'unicode'", null);
            String firstByName = DatabaseUtils.stringForQuery(db, "SELECT title FROM notes WHERE body <> 'unicode' ORDER BY title COLLATE LOCALIZED DESC LIMIT 1", null);
            db.delete("notes", "body = 'unicode'", null);
            detail = back + " " + length + " " + firstByName;
            return "Ёж € 😀".equals(back) && length == 6 && "gamma".equals(firstByName);
        });
        step("bad-sql", () -> {
            try {
                db.execSQL("SELEC nonsense FROM nowhere");
            } catch (SQLiteException e) {
                detail = e.getMessage();
                return e.getMessage() != null && e.getMessage().contains("syntax error");
            }
            return false;
        });
        step("memory", () -> {
            SQLiteDatabase mem = SQLiteDatabase.create(null);
            try {
                mem.execSQL("CREATE TABLE t (k TEXT, v INTEGER)");
                mem.execSQL("INSERT INTO t VALUES (?, ?)", new Object[] { "seven", 7 });
                mem.execSQL("INSERT INTO t VALUES (?, ?)", new Object[] { "eight", 8L });
                return DatabaseUtils.longForQuery(mem, "SELECT sum(v) FROM t WHERE k LIKE ?", new String[] { "%e%" }) == 15 && !new File(":memory:").exists();
            } finally {
                mem.close();
            }
        });
        step("upgrade", () -> {
            /* A second database, made at version 1 and opened again at version 2: onUpgrade once, and what it adds is there. */
            deleteDatabase("versions.db");
            Notes v1 = new Notes(this, "versions.db", 1);
            v1.getWritableDatabase().insertOrThrow("notes", null, note("old", "row", 1, 1.0, null));
            v1.close();
            Notes v2 = new Notes(this, "versions.db", 2);
            SQLiteDatabase d = v2.getReadableDatabase();
            String extra = DatabaseUtils.stringForQuery(d, "SELECT extra FROM notes WHERE title = 'old'", null);
            detail = "created=" + v1.created + "/" + v2.created + " upgraded=" + v2.upgraded + " version=" + d.getVersion() + " extra=" + extra;
            boolean held = v1.created == 1 && v2.created == 0 && v2.upgraded == 1 && d.getVersion() == 2 && "added".equals(extra);
            v2.close();
            return held && deleteDatabase("versions.db") && !getDatabasePath("versions.db").exists();
        });
        step("left", () -> {
            /* What the next run must find. */
            db.delete("notes", null, null);
            for (int i = 1; i <= 3; i++) db.insertOrThrow("notes", null, note("kept-" + i, "left by run " + run, i, i / 2.0, null));
            db.execSQL("INSERT INTO runs VALUES (" + run + ")");
            return count(db) == 3;
        });
        helper.close();
        storage(run);
        battery();
        timezone();
        tls();
        keystore(run);
        codecs();
        xml();
        assetFd();
        tasks();
        wrapperReceiver();
        viewGroupFlag();
        textDrawables();
        pfdDup();
        themeSelfRef();
        queueNext();
        storageStats();
        broadcastExplicit();
        network();
        Log.i("DataTest", "done ok=" + ok + " failed=" + failed);
        doneLogged = true;
        new android.os.Handler().postDelayed(this::popup, 700);
    }

    private android.view.View anchor;

    /**
     * A popup list that wraps its content, shown under the strip once the window is there: all its rows are inside it, clear of the
     * list's own padding. (Its window must be as high as the rows and what the list and the popup's background keep clear around them.)
     */
    private void popup() {
        final android.widget.ListPopupWindow popup = new android.widget.ListPopupWindow(this);
        popup.setAdapter(new android.widget.ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, new String[] { "one", "two", "three" }));
        popup.setAnchorView(anchor);
        popup.setWidth(300);
        popup.show();
        new android.os.Handler().postDelayed(() -> {
            android.widget.ListView list = popup.getListView();
            int rows = list == null ? -1 : list.getChildCount();
            android.view.View last = rows > 0 ? list.getChildAt(rows - 1) : null;
            boolean fits = rows == 3 && list.getChildAt(0).getTop() >= list.getPaddingTop() && last.getBottom() <= list.getHeight() - list.getPaddingBottom() && last.getHeight() > 0;
            Log.i("DataTest", "popup " + (fits ? "ok" : "FAILED") + " rows=" + rows + (list == null ? "" : " list=" + list.getWidth() + "x" + list.getHeight() + " padding=" + list.getPaddingTop()
                    + "+" + list.getPaddingBottom()) + (last == null ? "" : " row=" + last.getHeight() + " last-bottom=" + last.getBottom()));
            popup.dismiss();
        }, 700);
    }

    /**
     * The phone's storage as an app sees it: where things are (Environment, the app's own folders), what volumes there are
     * (StorageManager, the public way and the hidden one file managers use), how large it is (StatFs, StorageStatsManager),
     * that it may be used (permissions), and that it can be: a file written to Download is there to be read and listed, also by
     * the next run. Paths are checked as Android names them, not by where they are on the host.
     */
    private void storage(final long run) {
        final File root = android.os.Environment.getExternalStorageDirectory();
        step("environment", () -> {
            File download = android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS);
            detail = root + " " + download + " " + android.os.Environment.getExternalStorageState() + " " + android.os.Environment.getDataDirectory() + " "
                    + android.os.Environment.getRootDirectory() + " " + android.os.Environment.getDownloadCacheDirectory();
            return "/storage/emulated/0".equals(root.getPath()) && "/storage/emulated/0/Download".equals(download.getPath())
                    && "mounted".equals(android.os.Environment.getExternalStorageState()) && "mounted".equals(android.os.Environment.getExternalStorageState(download))
                    && android.os.Environment.isExternalStorageEmulated() && android.os.Environment.isExternalStorageEmulated(root)
                    && !android.os.Environment.isExternalStorageRemovable() && !android.os.Environment.isExternalStorageRemovable(root)
                    && "/data".equals(android.os.Environment.getDataDirectory().getPath()) && "/system".equals(android.os.Environment.getRootDirectory().getPath())
                    && "/data/cache".equals(android.os.Environment.getDownloadCacheDirectory().getPath()) && "Download".equals(android.os.Environment.DIRECTORY_DOWNLOADS)
                    && "DCIM".equals(android.os.Environment.DIRECTORY_DCIM) && "Documents".equals(android.os.Environment.DIRECTORY_DOCUMENTS);
        });
        step("own-folders", () -> {
            String base = "/storage/emulated/0/Android/";
            File files = getExternalFilesDir(null), pictures = getExternalFilesDir(android.os.Environment.DIRECTORY_PICTURES), cache = getExternalCacheDir(), obb = getObbDir();
            File[] media = getExternalMediaDirs(), all = getExternalFilesDirs(null);
            detail = files + " " + pictures + " " + cache + " " + obb + " " + Arrays.toString(media);
            /* Each is where Android puts it and is there once asked for; the application context and the activity agree. */
            return (base + "data/megler.test.data/files").equals(files.getPath()) && (base + "data/megler.test.data/files/Pictures").equals(pictures.getPath())
                    && (base + "data/megler.test.data/cache").equals(cache.getPath()) && (base + "obb/megler.test.data").equals(obb.getPath())
                    && media.length == 1 && (base + "media/megler.test.data").equals(media[0].getPath()) && all.length == 1 && all[0].equals(files)
                    && files.isDirectory() && pictures.isDirectory() && cache.isDirectory() && obb.isDirectory() && media[0].isDirectory()
                    && obb.equals(getApplicationContext().getObbDir()) && media[0].equals(getApplicationContext().getExternalMediaDirs()[0]);
        });
        step("volumes", () -> {
            android.os.storage.StorageManager sm = (android.os.storage.StorageManager) getSystemService(Context.STORAGE_SERVICE);
            java.util.List<android.os.storage.StorageVolume> list = sm.getStorageVolumes();
            android.os.storage.StorageVolume v = sm.getPrimaryStorageVolume();
            /* The hidden way, by reflection, as a file manager written before Android 7 finds the storage. */
            Object[] old = (Object[]) android.os.storage.StorageManager.class.getMethod("getVolumeList").invoke(sm);
            String[] paths = (String[]) android.os.storage.StorageManager.class.getMethod("getVolumePaths").invoke(sm);
            String state = (String) android.os.storage.StorageManager.class.getMethod("getVolumeState", String.class).invoke(sm, "/storage/emulated/0");
            String elsewhere = (String) android.os.storage.StorageManager.class.getMethod("getVolumeState", String.class).invoke(sm, "/storage/1234-5678");
            String path = (String) old[0].getClass().getMethod("getPath").invoke(old[0]);
            File pathFile = (File) old[0].getClass().getMethod("getPathFile").invoke(old[0]);
            detail = list.size() + " " + v.getDescription(this) + " " + path + " " + Arrays.toString(paths) + " " + state + " " + elsewhere + " " + v.getUuid();
            return list.size() == 1 && old.length == 1 && paths.length == 1 && "/storage/emulated/0".equals(path) && "/storage/emulated/0".equals(paths[0])
                    && root.equals(pathFile) && "mounted".equals(state) && "unknown".equals(elsewhere) && "Internal storage".equals(v.getDescription(this))
                    && v.isPrimary() && !v.isRemovable() && v.isEmulated() && "mounted".equals(v.getState()) && v.getUuid() == null
                    && sm.getStorageVolume(new File(root, "Download")) != null
                    && "41217664-9172-527a-b3d5-edabb50a7d69".equals(android.os.storage.StorageManager.UUID_DEFAULT.toString())
                    && android.os.storage.StorageManager.UUID_DEFAULT.equals(sm.getUuidForPath(getFilesDir()));
        });
        step("space", () -> {
            android.os.StatFs fs = new android.os.StatFs(root.getPath()), data = new android.os.StatFs(android.os.Environment.getDataDirectory().getPath());
            long total = fs.getTotalBytes(), free = fs.getAvailableBytes();
            android.app.usage.StorageStatsManager stats = (android.app.usage.StorageStatsManager) getSystemService(Context.STORAGE_STATS_SERVICE);
            long sold = stats.getTotalBytes(android.os.storage.StorageManager.UUID_DEFAULT), left = stats.getFreeBytes(android.os.storage.StorageManager.UUID_DEFAULT);
            detail = "total=" + total + " free=" + free + " blocks=" + fs.getBlockCountLong() + "x" + fs.getBlockSizeLong() + " int " + fs.getBlockCount() + "x" + fs.getBlockSize()
                    + " sold=" + sold + " left=" + left + " file " + root.getTotalSpace() + "/" + root.getUsableSpace();
            /* Sane, and the same whichever way it is asked: the long calls, the old int ones multiplied as longs, java.io.File, the data partition. */
            return total >= (1L << 30) && free > 0 && free <= total && fs.getBlockSizeLong() > 0 && fs.getBlockCountLong() * fs.getBlockSizeLong() == total
                    && (long) fs.getBlockCount() * fs.getBlockSize() == total && fs.getAvailableBlocksLong() * fs.getBlockSizeLong() == free
                    && (long) fs.getAvailableBlocks() * fs.getBlockSize() == free && fs.getFreeBytes() >= free && root.getTotalSpace() == total && root.getUsableSpace() == free
                    && data.getTotalBytes() >= (1L << 30)
                    /* What the phone is sold as: not less than what is there, less than twice it, and never nothing; and what is left of it. */
                    && sold >= data.getTotalBytes() && sold < 2 * data.getTotalBytes() + 1000 && left > 0 && left <= sold;
        });
        step("permissions", () -> {
            String read = android.Manifest.permission.READ_EXTERNAL_STORAGE, write = android.Manifest.permission.WRITE_EXTERNAL_STORAGE;
            int granted = android.content.pm.PackageManager.PERMISSION_GRANTED;
            requestPermissions(new String[] { read, write }, 77);
            return checkSelfPermission(read) == granted && checkSelfPermission(write) == granted && checkCallingOrSelfPermission(write) == granted
                    && checkPermission(write, android.os.Process.myPid(), android.os.Process.myUid()) == granted
                    && getPackageManager().checkPermission(write, getPackageName()) == granted && !shouldShowRequestPermissionRationale(write);
        });
        step("download", () -> {
            File dir = android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), file = new File(dir, "megler-data-test.txt");
            String before = null;
            if (file.exists()) {
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(file))) {
                    before = r.readLine();
                }
            }
            dir.mkdirs();
            try (java.io.FileWriter w = new java.io.FileWriter(file)) {
                w.write("written by run " + run + "\n");
            }
            String now;
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(file))) {
                now = r.readLine();
            }
            String[] names = dir.list();
            detail = "before=" + before + " now=" + now + " in " + Arrays.toString(names) + " by /sdcard: " + new File("/sdcard/Download/megler-data-test.txt").length();
            /* There, with what was written, listed in its folder; under the path's other name too; and the run before left its own. */
            return ("written by run " + run).equals(now) && file.length() == now.length() + 1 && names != null && Arrays.asList(names).contains("megler-data-test.txt")
                    && new File("/sdcard/Download/megler-data-test.txt").length() == file.length() && (run == 1 || ("written by run " + (run - 1)).equals(before));
        });
        step("list-root", () -> {
            File[] list = root.listFiles();
            java.util.ArrayList<String> names = new java.util.ArrayList<>();
            boolean dirs = list != null;
            if (list != null) for (File f : list) {
                names.add(f.getName());
                dirs &= f.isDirectory() || !isStandard(f.getName());
            }
            java.util.Collections.sort(names);
            detail = String.valueOf(names);
            /* The folders a phone's storage has from the start, each a folder. (Others may be there: the phone's own.) */
            return dirs && names.containsAll(Arrays.asList("Alarms", "Android", "DCIM", "Documents", "Download", "Movies", "Music", "Notifications", "Pictures", "Podcasts", "Ringtones"))
                    && root.isDirectory() && root.canRead() && root.canWrite() && new File(root, "Android/data/megler.test.data").isDirectory();
        });
    }

    private static boolean isStandard(String name) {
        return Arrays.asList("Alarms", "Android", "DCIM", "Documents", "Download", "Movies", "Music", "Notifications", "Pictures", "Podcasts", "Ringtones").contains(name);
    }

    /**
     * The battery, the two ways an app reads it: the sticky ACTION_BATTERY_CHANGED intent and BatteryManager. They must agree
     * with each other; what the numbers are is the test's to say, which sets them in the core (hk_battery_set) before the run.
     * A receiver registered for the intent is called with it, later.
     */
    private void battery() {
        step("battery", () -> {
            android.content.IntentFilter filter = new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED);
            android.content.Intent sticky = registerReceiver(null, filter);
            android.os.BatteryManager manager = (android.os.BatteryManager) getSystemService(Context.BATTERY_SERVICE);
            int capacity = manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY);
            Log.i("DataTest", "battery is " + describe(sticky) + " capacity=" + capacity + " charging=" + manager.isCharging());
            final android.content.Intent[] returned = new android.content.Intent[1];
            returned[0] = registerReceiver(new android.content.BroadcastReceiver() {
                private int calls;

                @Override
                public void onReceive(Context context, android.content.Intent intent) {
                    calls++;
                    boolean same = describe(intent).equals(describe(returned[0])) && android.content.Intent.ACTION_BATTERY_CHANGED.equals(intent.getAction());
                    Log.i("DataTest", "battery-received " + (same && calls == 1 && doneLogged && android.os.Looper.myLooper() == android.os.Looper.getMainLooper() ? "ok" : "FAILED"));
                    unregisterReceiver(this);
                }
            }, filter);
            int level = sticky.getIntExtra("level", -1), scale = sticky.getIntExtra("scale", -1);
            detail = describe(sticky) + " capacity=" + capacity;
            return level >= 0 && scale > 0 && level <= scale && capacity == level * 100 / scale && describe(sticky).equals(describe(returned[0]))
                    && manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_STATUS) == sticky.getIntExtra("status", -1)
                    && manager.isCharging() == (sticky.getIntExtra("plugged", -1) != 0 && (sticky.getIntExtra("status", -1) == 2 || sticky.getIntExtra("status", -1) == 5));
        });
        /*
         * And a receiver that stays, for what the test does to the battery once the steps are through (megler_set_battery): every broadcast
         * it hears is a line, numbered for the battery's own, and each says whether it came where a broadcast comes, on the main thread.
         */
        android.content.IntentFilter news = new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED);
        news.addAction(android.content.Intent.ACTION_POWER_CONNECTED);
        news.addAction(android.content.Intent.ACTION_POWER_DISCONNECTED);
        registerReceiver(new android.content.BroadcastReceiver() {
            private int heard;

            @Override
            public void onReceive(Context context, android.content.Intent intent) {
                String where = android.os.Looper.myLooper() == android.os.Looper.getMainLooper() ? "" : " OFF THE MAIN THREAD";
                if (android.content.Intent.ACTION_BATTERY_CHANGED.equals(intent.getAction())) Log.i("DataTest", "battery-heard " + (++heard) + where + " " + describe(intent));
                else Log.i("DataTest", "power-heard" + where + " " + intent.getAction());
            }
        }, news);
    }

    /**
     * The phone's time zone: two fixed instants, a winter's noon and a summer's by Greenwich, as the default zone has them. Which zone
     * that is the test says before the app starts (megler_set_timezone) and reads off the line logged here; what the step itself holds is
     * that the ways of asking agree: TimeZone, a date format, a Calendar and java.time.
     */
    private void timezone() {
        step("timezone", () -> {
            final long winter = 1673784000000L, summer = 1689422400000L;   /* 2023-01-15 and 2023-07-15, 12:00 in Greenwich */
            java.util.TimeZone zone = java.util.TimeZone.getDefault();
            java.text.SimpleDateFormat format = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm Z", java.util.Locale.US);
            String w = format.format(new java.util.Date(winter)), s = format.format(new java.util.Date(summer));
            Log.i("DataTest", "timezone is " + zone.getID() + " winter=" + w + " summer=" + s);
            java.util.Calendar calendar = java.util.Calendar.getInstance();
            calendar.setTimeInMillis(summer);
            java.time.ZonedDateTime zoned = java.time.Instant.ofEpochMilli(summer).atZone(java.time.ZoneId.systemDefault());
            detail = zone.getID() + " " + s + " calendar " + calendar.getTimeZone().getID() + " " + calendar.get(java.util.Calendar.HOUR_OF_DAY) + " java.time " + zoned;
            return format.getTimeZone().getID().equals(zone.getID()) && calendar.getTimeZone().getID().equals(zone.getID())
                    && s.contains(String.format(java.util.Locale.US, " %02d:00 ", calendar.get(java.util.Calendar.HOUR_OF_DAY)))
                    && zoned.getHour() == calendar.get(java.util.Calendar.HOUR_OF_DAY) && zoned.getOffset().getTotalSeconds() * 1000 == zone.getOffset(summer)
                    && zone.getOffset(winter) == zone.getRawOffset();
        });
    }

    /** The test server's key and certificate: EC P-256, for 127.0.0.1 and localhost, its own issuer, valid until 2126 (openssl req -x509). */
    private static final String TLS_CERT = "MIIBmzCCAUGgAwIBAgIUSCkkSCVXmr4YGF4nBH/UqsiQrrEwCgYIKoZIzj0EAwIwFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAxNDE5NDkwN1oYDzIx"
            + "MjYwOTIwMTk0OTA3WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAASSOKxorFlV2PxAaplmg0VzCo6g5dcgejzgpERSfmqr"
            + "UCxTzAdCDQLxcLOZCYgMKwUHK2eyrVByyAtXT+l2f5t3o28wbTAdBgNVHQ4EFgQUlQdcW7Xv7E4n8iyupv2dBZoWQg0wHwYDVR0jBBgwFoAUlQdcW7Xv7E4n8iyu"
            + "pv2dBZoWQg0wDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwCgYIKoZIzj0EAwIDSAAwRQIhANZj3S08wMis2aZyDna2u0yD7uCg"
            + "z9HYDYREfE1Ss5ArAiA+Y1r8tv+lwWIh8fhqvc3nS+gF3Y67jscJtaox+zq1/Q==";
    private static final String TLS_KEY = "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgxyjM+eEJTgNZNj8S0HvPUp/EYUsW7SCywlDYtxPU9QKhRANCAASSOKxorFlV2PxAaplm"
            + "g0VzCo6g5dcgejzgpERSfmqrUCxTzAdCDQLxcLOZCYgMKwUHK2eyrVByyAtXT+l2f5t3";

    /** Whether the TLS this app has is conscrypt's (its native library is in the Android root) or the runtime's stand-in, which has none. */
    private boolean conscrypt;

    /**
     * TLS. With conscrypt's native library in the Android root the security providers are Android's own and TLS is conscrypt's,
     * over BoringSSL: the platform trusts Android 9's certificate authorities and nothing else, and the two steps after this one
     * talk TLS on loopback. Without it the runtime's stand-ins are there (NoTlsContext.smali): the contexts an app and Android's
     * own HTTP client ask for are there, and a connection that would use one fails as a connection fails -- an IOException -- not
     * with the AssertionError Android's client throws when there is no SSLContext at all, which ended threads of MX Player and
     * Super S22 Launcher. Which of the two it is, the log says ("tls-is"), and the test holds it against the Android root.
     */
    private void tls() {
        step("tls", () -> {
            javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
            context.init(null, null, null);
            String provider = context.getProvider().getClass().getName();
            conscrypt = "com.android.org.conscrypt.OpenSSLProvider".equals(provider);
            Log.i("DataTest", "tls-is " + (conscrypt ? "conscrypt" : "none") + " provider=" + provider);
            javax.net.ssl.SSLSocketFactory factory = context.getSocketFactory();
            boolean there = factory != null && javax.net.ssl.SSLContext.getInstance("TLSv1.2") != null && javax.net.ssl.SSLContext.getDefault() != null
                    && javax.net.ssl.SSLSocketFactory.getDefault() != null && javax.net.ssl.HttpsURLConnection.getDefaultSSLSocketFactory() != null;
            if (conscrypt) return there && tlsPlatform();
            String direct;
            try {
                factory.createSocket("127.0.0.1", 443).close();
                direct = "made";
            } catch (java.io.IOException e) {
                direct = e.getMessage();
            }
            /* An https URL as an app opens one. Nothing listens there; whichever way it fails, it must fail as an IOException. */
            String https;
            try {
                java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("https://127.0.0.1:9/").openConnection();
                c.setConnectTimeout(3000);
                c.connect();
                https = "connected";
            } catch (java.io.IOException e) {
                https = "IOException";
            }
            /* The platform's trust managers, which an HTTP client of an app's own asks for first: one, trusting nothing. */
            javax.net.ssl.TrustManagerFactory trust = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
            trust.init((java.security.KeyStore) null);
            javax.net.ssl.TrustManager[] managers = trust.getTrustManagers();
            String trusts;
            try {
                ((javax.net.ssl.X509TrustManager) managers[0]).checkServerTrusted(new java.security.cert.X509Certificate[0], "RSA");
                trusts = "trusted";
            } catch (java.security.cert.CertificateException e) {
                trusts = e.getMessage();
            }
            detail = "there=" + there + " direct=" + direct + " https=" + https + " trust=" + managers.length + " " + trusts;
            return there && "no TLS on this phone yet".equals(direct) && "IOException".equals(https) && managers.length == 1
                    && managers[0] instanceof javax.net.ssl.X509TrustManager && "no TLS on this phone yet".equals(trusts)
                    && ((javax.net.ssl.X509TrustManager) managers[0]).getAcceptedIssuers().length == 0;
        });
        /* Without TLS there is nothing to talk on loopback: the two steps say so and hold. */
        step("tls-loopback", () -> !conscrypt || offMain(this::tlsLoopback));
        step("tls-https", () -> !conscrypt || offMain(this::tlsHttps));
    }

    /**
     * conscrypt's platform side: digests are its own (over BoringSSL), the platform's trust manager holds Android 9's certificate
     * authorities, a certificate none of them issued is refused, and an https URL nobody listens at fails as an IOException.
     */
    private boolean tlsPlatform() throws Exception {
        String digests = java.security.MessageDigest.getInstance("SHA-256").getProvider().getClass().getName();
        javax.net.ssl.TrustManagerFactory trust = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
        trust.init((java.security.KeyStore) null);
        javax.net.ssl.TrustManager[] managers = trust.getTrustManagers();
        javax.net.ssl.X509TrustManager platform = managers.length == 1 && managers[0] instanceof javax.net.ssl.X509TrustManager ? (javax.net.ssl.X509TrustManager) managers[0] : null;
        int issuers = platform == null ? -1 : platform.getAcceptedIssuers().length;
        String trusts;
        try {
            platform.checkServerTrusted(new java.security.cert.X509Certificate[] { tlsCertificate() }, "ECDHE_ECDSA");
            trusts = "trusted";
        } catch (java.security.cert.CertificateException e) {
            trusts = "refused";
        }
        String https;
        try {
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("https://127.0.0.1:9/").openConnection();
            c.setConnectTimeout(3000);
            c.connect();
            https = "connected";
        } catch (java.io.IOException e) {
            https = "IOException";
        }
        detail = "digests=" + digests + " trust=" + managers.length + " issuers=" + issuers + " self-signed=" + trusts + " https=" + https;
        Log.i("DataTest", "tls-said " + detail);
        return "com.android.org.conscrypt.OpenSSLProvider".equals(digests) && platform != null && issuers >= 100 && "refused".equals(trusts) && "IOException".equals(https);
    }

    private static java.security.cert.X509Certificate tlsCertificate() throws Exception {
        return (java.security.cert.X509Certificate) java.security.cert.CertificateFactory.getInstance("X.509")
                .generateCertificate(new java.io.ByteArrayInputStream(java.util.Base64.getDecoder().decode(TLS_CERT)));
    }

    /** [0]: a server's context with the test key, [1]: a client's that trusts the test certificate alone. */
    private static javax.net.ssl.SSLContext[] tlsContexts() throws Exception {
        java.security.cert.X509Certificate cert = tlsCertificate();
        java.security.PrivateKey key = java.security.KeyFactory.getInstance("EC").generatePrivate(new java.security.spec.PKCS8EncodedKeySpec(java.util.Base64.getDecoder().decode(TLS_KEY)));
        java.security.KeyStore keys = java.security.KeyStore.getInstance(java.security.KeyStore.getDefaultType());
        keys.load(null, null);
        keys.setKeyEntry("server", key, new char[0], new java.security.cert.Certificate[] { cert });
        javax.net.ssl.KeyManagerFactory kmf = javax.net.ssl.KeyManagerFactory.getInstance(javax.net.ssl.KeyManagerFactory.getDefaultAlgorithm());
        kmf.init(keys, new char[0]);
        java.security.KeyStore trusted = java.security.KeyStore.getInstance(java.security.KeyStore.getDefaultType());
        trusted.load(null, null);
        trusted.setCertificateEntry("server", cert);
        javax.net.ssl.TrustManagerFactory tmf = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
        tmf.init(trusted);
        javax.net.ssl.SSLContext server = javax.net.ssl.SSLContext.getInstance("TLS"), client = javax.net.ssl.SSLContext.getInstance("TLS");
        server.init(kmf.getKeyManagers(), null, null);
        client.init(null, tmf.getTrustManagers(), null);
        return new javax.net.ssl.SSLContext[] { server, client };
    }

    /**
     * A TLS server of the app's own on 127.0.0.1 and a client that trusts its certificate: the handshake, a line each way, the
     * session's protocol, suite and peer. Then a client with the platform's trust, which must refuse the same server, and the
     * server must see the handshake fail.
     */
    private boolean tlsLoopback() throws Exception {
        javax.net.ssl.SSLContext[] contexts = tlsContexts();
        final javax.net.ssl.SSLServerSocket server = (javax.net.ssl.SSLServerSocket) contexts[0].getServerSocketFactory()
                .createServerSocket(0, 5, java.net.InetAddress.getByName("127.0.0.1"));
        final String[] heard = new String[2];
        Thread t = new Thread(() -> {
            for (int i = 0; i < 2; i++) {
                try (java.net.Socket c = server.accept()) {
                    c.setSoTimeout(10000);
                    heard[i] = new java.io.BufferedReader(new java.io.InputStreamReader(c.getInputStream(), "UTF-8")).readLine();
                    c.getOutputStream().write("pong\n".getBytes("UTF-8"));
                    c.getOutputStream().flush();
                } catch (java.io.IOException e) {
                    heard[i] = "the server: " + e.getClass().getSimpleName();
                }
            }
        }, "tls-server");
        t.start();
        String back, protocol, suite;
        boolean peer;
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) contexts[1].getSocketFactory().createSocket("127.0.0.1", server.getLocalPort())) {
            s.setSoTimeout(10000);
            s.startHandshake();
            javax.net.ssl.SSLSession session = s.getSession();
            protocol = session.getProtocol();
            suite = session.getCipherSuite();
            peer = session.getPeerCertificates()[0].equals(tlsCertificate());
            s.getOutputStream().write("ping\n".getBytes("UTF-8"));
            s.getOutputStream().flush();
            back = new java.io.BufferedReader(new java.io.InputStreamReader(s.getInputStream(), "UTF-8")).readLine();
        }
        String refused;
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) javax.net.ssl.SSLSocketFactory.getDefault().createSocket("127.0.0.1", server.getLocalPort())) {
            s.setSoTimeout(10000);
            s.startHandshake();
            refused = "handshake made";
        } catch (javax.net.ssl.SSLHandshakeException e) {
            refused = "SSLHandshakeException";
        }
        t.join(10000);
        server.close();
        detail = "protocol=" + protocol + " suite=" + suite + " peer=" + peer + " heard=" + heard[0] + " back=" + back + " platform=" + refused + " then " + heard[1];
        Log.i("DataTest", "tls-loopback-said " + detail);
        return !t.isAlive() && "TLSv1.2".equals(protocol) && suite.startsWith("TLS_ECDHE_ECDSA_") && peer && "ping".equals(heard[0]) && "pong".equals(back)
                && "SSLHandshakeException".equals(refused) && heard[1] != null && heard[1].startsWith("the server: ");
    }

    /**
     * An https URL as an app opens one, through Android's own HTTP client (com.android.okhttp, behind URL.openConnection), by name:
     * "localhost", which the certificate names, so the host name is checked too.
     */
    private boolean tlsHttps() throws Exception {
        javax.net.ssl.SSLContext[] contexts = tlsContexts();
        final javax.net.ssl.SSLServerSocket server = (javax.net.ssl.SSLServerSocket) contexts[0].getServerSocketFactory()
                .createServerSocket(0, 5, java.net.InetAddress.getByName("127.0.0.1"));
        final String[] request = new String[1];
        Thread t = new Thread(() -> {
            try (java.net.Socket c = server.accept()) {
                c.setSoTimeout(10000);
                java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(c.getInputStream(), "ISO-8859-1"));
                request[0] = r.readLine();
                for (String line; (line = r.readLine()) != null && !line.isEmpty();) { }
                java.io.OutputStream o = c.getOutputStream();
                o.write("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nmegler!".getBytes("ISO-8859-1"));
                o.flush();
            } catch (java.io.IOException e) {
                request[0] = "the server: " + e;
            }
        }, "tls-https-server");
        t.start();
        javax.net.ssl.HttpsURLConnection c = (javax.net.ssl.HttpsURLConnection) new java.net.URL("https://localhost:" + server.getLocalPort() + "/hello").openConnection();
        c.setSSLSocketFactory(contexts[1].getSocketFactory());
        c.setConnectTimeout(5000);
        c.setReadTimeout(10000);
        int code = c.getResponseCode();
        String suite = c.getCipherSuite(), body = readAll(c.getInputStream());
        c.disconnect();
        t.join(10000);
        server.close();
        detail = "code=" + code + " suite=" + suite + " body=" + body + " request=" + request[0];
        Log.i("DataTest", "tls-https-said " + detail);
        return code == 200 && "megler!".equals(body) && suite != null && suite.startsWith("TLS_ECDHE_ECDSA_") && "GET /hello HTTP/1.1".equals(request[0]);
    }

    /**
     * Android's key store as Amazon's sign-in library (in MX Player) uses it: an RSA pair made under an alias by
     * KeyPairGenerator.getInstance("RSA", "AndroidKeyStore"), a key of sixteen bytes wrapped with its public key and unwrapped with
     * its private one, both found again through KeyStore.getInstance("AndroidKeyStore"); an EC pair the same way, which signs; and
     * the RSA pair is there for the next run, which finds it before it makes it again.
     */
    private void keystore(final long run) {
        step("keystore", () -> {
            java.security.KeyStore store = java.security.KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            boolean before = store.containsAlias("megler-test-rsa");
            java.security.KeyPairGenerator rsa = java.security.KeyPairGenerator.getInstance("RSA", "AndroidKeyStore");
            rsa.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-rsa", android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                    | android.security.keystore.KeyProperties.PURPOSE_DECRYPT).setKeySize(2048)
                    .setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_RSA_PKCS1).build());
            java.security.KeyPair pair = rsa.generateKeyPair();
            byte[] secret = "sixteen byte key".getBytes("UTF-8");
            javax.crypto.Cipher wrap = javax.crypto.Cipher.getInstance("RSA/ECB/PKCS1Padding");
            wrap.init(javax.crypto.Cipher.ENCRYPT_MODE, store.getCertificate("megler-test-rsa").getPublicKey());
            byte[] wrapped = wrap.doFinal(secret);
            javax.crypto.Cipher unwrap = javax.crypto.Cipher.getInstance("RSA/ECB/PKCS1Padding");
            unwrap.init(javax.crypto.Cipher.DECRYPT_MODE, (java.security.PrivateKey) store.getKey("megler-test-rsa", null));
            int bits = ((java.security.interfaces.RSAPublicKey) pair.getPublic()).getModulus().bitLength();
            boolean rsaHeld = Arrays.equals(secret, unwrap.doFinal(wrapped)) && wrapped.length == 256 && bits == 2048;

            java.security.KeyPairGenerator ec = java.security.KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
            ec.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-ec", android.security.keystore.KeyProperties.PURPOSE_SIGN
                    | android.security.keystore.KeyProperties.PURPOSE_VERIFY).setDigests(android.security.keystore.KeyProperties.DIGEST_SHA256).build());
            java.security.KeyPair ecPair = ec.generateKeyPair();
            java.security.Signature sign = java.security.Signature.getInstance("SHA256withECDSA");
            sign.initSign((java.security.PrivateKey) store.getKey("megler-test-ec", null));
            sign.update(secret);
            byte[] signature = sign.sign();
            java.security.Signature verify = java.security.Signature.getInstance("SHA256withECDSA");
            verify.initVerify(store.getCertificate("megler-test-ec").getPublicKey());
            verify.update(secret);
            int ecBits = ((java.security.interfaces.ECPublicKey) ecPair.getPublic()).getParams().getCurve().getField().getFieldSize();
            boolean ecHeld = verify.verify(signature) && ecBits == 256;
            detail = "rsa=" + rsaHeld + " (" + bits + " bits, " + wrapped.length + " bytes wrapped) ec=" + ecHeld + " (" + ecBits + " bits) there-before=" + before + " run=" + run;
            Log.i("DataTest", "keystore " + detail);
            return rsaHeld && ecHeld && before == (run > 1);
        });
    }

    /**
     * The decoders an app can have, which is none yet (the shell decodes whole files, not an app's samples): the list is empty
     * every way it is asked for, nothing is found for a film's format, and asking for a decoder by type is the IOException
     * Android throws when there is none -- nothing that ends an app.
     */
    private void codecs() {
        step("codecs", () -> {
            int count = android.media.MediaCodecList.getCodecCount();
            android.media.MediaCodecList list = new android.media.MediaCodecList(android.media.MediaCodecList.ALL_CODECS);
            int all = list.getCodecInfos().length;
            String found = list.findDecoderForFormat(android.media.MediaFormat.createVideoFormat("video/avc", 640, 360));
            String made;
            try {
                android.media.MediaCodec.createDecoderByType("video/avc").release();
                made = "made";
            } catch (java.io.IOException e) {
                made = "IOException";
            }
            detail = "count=" + count + " all=" + all + " avc=" + found + " create=" + made;
            return count == 0 && all == 0 && found == null && "IOException".equals(made);
        });
    }

    /**
     * XML through android.util.Xml, as apps read and write their own files and settings with it: a serializer writes a small
     * document with a namespace, an attribute and escaped text, and a pull parser made by Xml.newPullParser reads it back. Both
     * are kXML's, under the name Android 9's libcore gives them (MX Player ended at its start when the framework asked for
     * Android 10's).
     */
    private void xml() {
        step("xml", () -> {
            java.io.StringWriter out = new java.io.StringWriter();
            org.xmlpull.v1.XmlSerializer w = android.util.Xml.newSerializer();
            w.setOutput(out);
            w.startDocument("UTF-8", true);
            w.setPrefix("h", "urn:megler");
            w.startTag("urn:megler", "list");
            w.startTag(null, "item").attribute(null, "n", "1").text("a < b & c").endTag(null, "item");
            w.endTag("urn:megler", "list");
            w.endDocument();
            String text = out.toString();
            org.xmlpull.v1.XmlPullParser p = android.util.Xml.newPullParser();
            p.setInput(new java.io.StringReader(text));
            StringBuilder seen = new StringBuilder();
            for (int e = p.getEventType(); e != org.xmlpull.v1.XmlPullParser.END_DOCUMENT; e = p.next()) {
                if (e == org.xmlpull.v1.XmlPullParser.START_TAG) seen.append('<').append(p.getName()).append(p.getAttributeCount() > 0 ? "@" + p.getAttributeValue(null, "n") : "").append(p.getNamespace().isEmpty() ? "" : "{" + p.getNamespace() + "}");
                else if (e == org.xmlpull.v1.XmlPullParser.TEXT) seen.append('"').append(p.getText()).append('"');
                else if (e == org.xmlpull.v1.XmlPullParser.END_TAG) seen.append("</").append(p.getName());
            }
            detail = p.getClass().getName() + " " + w.getClass().getName() + " " + seen;
            return "<list{urn:megler}<item@1\"a < b & c\"</item</list".equals(seen.toString()) && text.contains("a &lt; b &amp; c");
        });
    }

    /**
     * A file of the APK from its root as a descriptor, the way Chromium's ApkAssets maps its data out of the APK:
     * openNonAssetFd, then the number taken from getParcelFileDescriptor().detachFd(). resources.arsc is stored, so it can
     * be had so; it begins with its table's header (type 2, header size 12).
     */
    private void assetFd() {
        step("asset-fd", () -> {
            android.content.res.AssetFileDescriptor afd = getAssets().openNonAssetFd("resources.arsc");
            android.os.ParcelFileDescriptor pfd = afd.getParcelFileDescriptor();
            byte[] head = new byte[4];
            java.io.FileInputStream in = new java.io.FileInputStream(afd.getFileDescriptor());
            in.getChannel().position(afd.getStartOffset());
            int got = in.read(head);
            int fd = pfd != null ? pfd.detachFd() : -1;
            detail = "pfd=" + (pfd != null) + " fd=" + fd + " at " + afd.getStartOffset() + " length " + afd.getLength() + " head " + java.util.Arrays.toString(head);
            return pfd != null && fd >= 0 && got == 4 && head[0] == 2 && head[1] == 0 && head[2] == 12 && head[3] == 0 && afd.getLength() > 12;
        });
    }

    /**
     * registerReceiver with a permission and a handler, or flags, through a ContextWrapper whose two-argument form throws,
     * as Chromium's does: each form goes to the wrapped context, as on Android, not through the two-argument one.
     */
    private void wrapperReceiver() {
        step("wrapper-receiver", () -> {
            android.content.ContextWrapper w = new android.content.ContextWrapper(this) {
                @Override public android.content.Intent registerReceiver(android.content.BroadcastReceiver r, android.content.IntentFilter f) {
                    throw new RuntimeException("the two-argument form");
                }
            };
            android.content.BroadcastReceiver r = new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) { }
            };
            android.content.IntentFilter f = new android.content.IntentFilter("megler.test.WRAPPED");
            w.registerReceiver(r, f, null, null);
            w.registerReceiver(r, new android.content.IntentFilter("megler.test.WRAPPED2"), null, null, 0);
            w.registerReceiver(r, new android.content.IntentFilter("megler.test.WRAPPED3"), 0);
            w.unregisterReceiver(r);
            detail = "registered and unregistered through the wrapper";
            return true;
        });
    }

    /**
     * An EditText's cursor and selection handles (API 29), which Chromium's omnibox tints: none of the four is null, each
     * takes a tint, and what is set is what is given back.
     */
    private void textDrawables() {
        step("text-drawables", () -> {
            android.widget.EditText e = new android.widget.EditText(this);
            java.lang.reflect.Method[] get = new java.lang.reflect.Method[4];
            String[] names = { "getTextCursorDrawable", "getTextSelectHandle", "getTextSelectHandleLeft", "getTextSelectHandleRight" };
            StringBuilder seen = new StringBuilder();
            boolean held = true;
            for (int i = 0; i < 4; i++) {
                android.graphics.drawable.Drawable d = (android.graphics.drawable.Drawable) android.widget.TextView.class.getMethod(names[i]).invoke(e);
                seen.append(names[i]).append('=').append(d == null ? "null" : d.getClass().getSimpleName()).append(' ');
                if (d == null) held = false;
                else d.mutate().setTint(0xFF00FF00);
            }
            android.graphics.drawable.ColorDrawable mine = new android.graphics.drawable.ColorDrawable(0xFF0000FF);
            android.widget.TextView.class.getMethod("setTextSelectHandleLeft", android.graphics.drawable.Drawable.class).invoke(e, mine);
            held &= android.widget.TextView.class.getMethod("getTextSelectHandleLeft").invoke(e) == mine;
            detail = seen.toString();
            return held;
        });
    }

    /**
     * ParcelFileDescriptor's descriptors, as GeckoView hands its crash helper a socket pair's ends: dup makes a descriptor
     * of its own, which outlives the original's close and writes where it did; detachFd gives the number away, so that
     * closing the ParcelFileDescriptor leaves it; adoptFd takes it over, and closing that closes it.
     */
    private void pfdDup() {
        step("pfd-dup", () -> {
            java.io.FileDescriptor a = new java.io.FileDescriptor(), b = new java.io.FileDescriptor();
            android.system.Os.socketpair(android.system.OsConstants.AF_UNIX, android.system.OsConstants.SOCK_SEQPACKET, 0, a, b);
            android.os.ParcelFileDescriptor da = android.os.ParcelFileDescriptor.dup(a);
            android.system.Os.close(a);
            java.io.File file = new java.io.File(getCacheDir(), "pfd-dup.txt");
            java.io.FileOutputStream out = new java.io.FileOutputStream(file);
            android.os.ParcelFileDescriptor d = android.os.ParcelFileDescriptor.dup(out.getFD());
            out.close();
            new java.io.FileOutputStream(d.getFileDescriptor()).write("kept".getBytes("UTF-8"));
            int n = d.detachFd();
            d.close();
            android.os.ParcelFileDescriptor adopted = android.os.ParcelFileDescriptor.adoptFd(n);
            boolean open = adopted.getFileDescriptor().valid();
            adopted.close();
            int na = da.detachFd();
            detail = "socket end " + na + ", file " + n + " (" + file.length() + " bytes), open before adoptFd's close " + open
                + ", after " + adopted.getFileDescriptor().valid();
            android.system.Os.close(b);
            return na >= 0 && n >= 0 && file.length() == 4 && open && !adopted.getFileDescriptor().valid() && file.delete();
        });
    }

    /**
     * A theme with android.R.style.TextAppearance applied, as GeckoView asks the system's colours: TextAppearance's
     * textColorHighlight names itself, which leaves it out rather than an attribute getColor cannot take; every colour that
     * is there can be had.
     */
    private void themeSelfRef() {
        step("theme-selfref", () -> {
            android.view.ContextThemeWrapper w = new android.view.ContextThemeWrapper(getApplicationContext(), android.R.style.TextAppearance);
            int[] attrs = { android.R.attr.textColorPrimary, android.R.attr.textColorHighlight, android.R.attr.colorForeground };
            android.content.res.TypedArray a = w.getTheme().obtainStyledAttributes(attrs);
            StringBuilder seen = new StringBuilder();
            for (int i = 0; i < a.getIndexCount(); i++) {
                int at = a.getIndex(i);
                seen.append(at).append('=').append(Integer.toHexString(a.getColor(at, 0))).append(' ');
            }
            boolean highlight = a.hasValue(1);
            a.recycle();
            detail = seen + "highlight there=" + highlight;
            return true;
        });
    }

    /**
     * MessageQueue.next(), hidden API, as Gecko takes the Java looper's messages on its own thread: a message sent on a
     * looper thread of the step's own comes back from it.
     */
    private void queueNext() {
        step("queue-next", () -> {
            final Object[] got = new Object[1];
            Thread t = new Thread(() -> {
                android.os.Looper.prepare();
                new android.os.Handler().sendEmptyMessage(42);
                try {
                    java.lang.reflect.Method next = android.os.MessageQueue.class.getDeclaredMethod("next");
                    next.setAccessible(true);
                    android.os.Message m = (android.os.Message) next.invoke(android.os.Looper.myQueue());
                    got[0] = m == null ? null : m.what;
                } catch (Throwable e) {
                    got[0] = e;
                }
            }, "queue-next");
            t.start();
            t.join(5000);
            detail = "next() gave " + got[0];
            return Integer.valueOf(42).equals(got[0]);
        });
    }

    /** The app's own storage statistics, as Firefox times them for its telemetry: there, not null. */
    private void storageStats() {
        step("storage-stats", () -> {
            android.app.usage.StorageStatsManager m = (android.app.usage.StorageStatsManager) getSystemService(STORAGE_STATS_SERVICE);
            android.app.usage.StorageStats s = m.queryStatsForUid(android.os.storage.StorageManager.UUID_DEFAULT, android.os.Process.myUid());
            detail = "queryStatsForUid gave " + s;
            return s != null && s.getAppBytes() >= 0 && s.getDataBytes() >= 0;
        });
    }

    static int relayHeard, targetHeard, offHeard;

    /** A receiver of the manifest for megler.test.RELAY, which passes what it hears to Target by an intent that names Target. */
    public static final class Relay extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, android.content.Intent i) {
            /* Bounded, so that a framework that brings the relayed intent back here does not keep the main thread at it. */
            if (++relayHeard < 50) c.sendBroadcast(new android.content.Intent(i.getAction()).setComponent(new android.content.ComponentName(c, "megler.test.data.Main$Target")));
        }
    }

    /** A receiver of the manifest without a filter: it hears what names it. */
    public static final class Target extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, android.content.Intent i) { targetHeard++; }
    }

    /** A receiver the manifest disables, with Relay's filter: it hears nothing. */
    public static final class Off extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, android.content.Intent i) { offHeard++; }
    }

    /**
     * Who hears a broadcast. An implicit one reaches the manifest's receivers whose filters take it and the receivers registered
     * for it; one that names a component reaches that component alone: Relay's relay reaches Target, not Relay again, not the
     * receiver registered for the same action. A disabled receiver hears neither. Counted once the deliveries are through.
     */
    private void broadcastExplicit() {
        step("broadcast-explicit", () -> {
            final int[] registered = new int[1];
            final android.content.BroadcastReceiver r = new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) { registered[0]++; }
            };
            registerReceiver(r, new android.content.IntentFilter("megler.test.RELAY"));
            sendBroadcast(new android.content.Intent("megler.test.RELAY"));
            new android.os.Handler().postDelayed(() -> {
                unregisterReceiver(r);
                boolean held = relayHeard == 1 && targetHeard == 1 && offHeard == 0 && registered[0] == 1;
                Log.i("DataTest", "broadcast-relay " + (held ? "ok" : "FAILED") + " relay=" + relayHeard + " target=" + targetHeard + " off=" + offHeard + " registered=" + registered[0]);
            }, 1500);
            return true;
        });
    }

    /** A FrameLayout whose setWillNotDraw needs what its own constructor makes, as Chromium's CompositorView does. */
    private static final class WillNotDrawProbe extends android.widget.FrameLayout {
        private final Object made = new Object();
        int calls;
        WillNotDrawProbe(android.content.Context c) { super(c); }
        @Override public void setWillNotDraw(boolean w) { made.hashCode(); calls++; super.setWillNotDraw(w); }
    }

    /** A ViewGroup's constructor marks it as not drawing itself without calling setWillNotDraw, which a subclass may override. */
    private void viewGroupFlag() {
        step("viewgroup-flag", () -> {
            WillNotDrawProbe p = new WillNotDrawProbe(this);
            detail = "willNotDraw=" + p.willNotDraw() + " calls=" + p.calls;
            return p.willNotDraw() && p.calls == 0;
        });
    }

    /**
     * The app's task as ActivityManager.getAppTasks() describes it, through the fields RecentTaskInfo has from TaskInfo
     * (API 29), which Chromium reads: this activity, the only one, at its root and on its top.
     */
    private void tasks() {
        step("tasks", () -> {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            java.util.List<android.app.ActivityManager.AppTask> tasks = am.getAppTasks();
            android.app.ActivityManager.RecentTaskInfo t = tasks.isEmpty() ? null : tasks.get(0).getTaskInfo();
            if (t == null) return false;
            detail = "task " + t.taskId + " of " + t.numActivities + ", base " + t.baseActivity + ", top " + t.topActivity + ", running " + t.isRunning;
            return t.taskId == getTaskId() && t.isRunning && t.numActivities == 1 && t.baseActivity != null
                && t.baseActivity.getClassName().equals(Main.class.getName()) && t.baseActivity.equals(t.topActivity)
                && t.baseIntent != null && t.baseIntent.getComponent() != null;
        });
    }

    /** Work a step hands to a thread of its own, as an app hands the network to one: the step waits for it. */
    private boolean offMain(final Step work) throws Throwable {
        final boolean[] held = new boolean[1];
        final Throwable[] thrown = new Throwable[1];
        Thread t = new Thread(() -> {
            try {
                held[0] = work.run();
            } catch (Throwable e) {
                thrown[0] = e;
            }
        }, "net-step");
        t.start();
        t.join(20000);
        if (t.isAlive()) {
            detail = (detail == null ? "" : detail + " ") + "still running after 20 s";
            return false;
        }
        if (thrown[0] != null) throw thrown[0];
        return held[0];
    }

    private static String readAll(java.io.InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[256];
        for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        return new String(out.toByteArray(), "UTF-8");
    }

    /**
     * The network as Java code uses it, on this device's own loopback: names, an HTTP server of the app's own answering
     * HttpURLConnection, UDP, a server socket closed under the thread that waits in accept (libcore's way of ending that wait), the
     * interfaces, and what ConnectivityManager says. Nothing leaves the device.
     */
    private void network() {
        step("net-localhost", () -> offMain(() -> {
            java.net.InetAddress a = java.net.InetAddress.getByName("localhost");
            java.net.InetAddress[] all = java.net.InetAddress.getAllByName("localhost");
            java.net.InetAddress six = java.net.InetAddress.getByName("::1"), four = java.net.InetAddress.getByName("10.1.2.3");
            detail = a + " " + Arrays.toString(all) + " " + six + " " + four;
            return a.isLoopbackAddress() && "127.0.0.1".equals(a.getHostAddress()) && all.length >= 1 && all[0].equals(a)
                    && six instanceof java.net.Inet6Address && six.isLoopbackAddress() && four instanceof java.net.Inet4Address
                    && "10.1.2.3".equals(four.getHostAddress());
        }));
        step("net-http", () -> offMain(() -> {
            final java.net.ServerSocket server = new java.net.ServerSocket(0, 5, java.net.InetAddress.getByName("127.0.0.1"));
            final String[] request = new String[1];
            Thread t = new Thread(() -> {
                try (java.net.Socket c = server.accept()) {
                    java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(c.getInputStream(), "ISO-8859-1"));
                    request[0] = r.readLine();
                    for (String line; (line = r.readLine()) != null && !line.isEmpty();) { }
                    java.io.OutputStream o = c.getOutputStream();
                    o.write("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nmegler!".getBytes("ISO-8859-1"));
                    o.flush();
                } catch (java.io.IOException e) {
                    request[0] = "the server: " + e;
                }
            }, "net-server");
            t.start();
            /* By name: HttpURLConnection looks "localhost" up as an app's URL would be. */
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("http://localhost:" + server.getLocalPort() + "/hello").openConnection();
            c.setConnectTimeout(5000);
            c.setReadTimeout(5000);
            int code = c.getResponseCode();
            String body = readAll(c.getInputStream()), type = c.getContentType();
            c.disconnect();
            t.join(5000);
            server.close();
            detail = "code=" + code + " type=" + type + " body=" + body + " request=" + request[0];
            return code == 200 && "megler!".equals(body) && "text/plain".equals(type) && "GET /hello HTTP/1.1".equals(request[0]);
        }));
        step("net-udp", () -> offMain(() -> {
            java.net.InetAddress lo = java.net.InetAddress.getByName("127.0.0.1");
            try (java.net.DatagramSocket a = new java.net.DatagramSocket(0, lo); java.net.DatagramSocket b = new java.net.DatagramSocket(0, lo)) {
                byte[] buf = new byte[64];
                a.setSoTimeout(5000);
                b.setSoTimeout(5000);
                a.send(new java.net.DatagramPacket("ping".getBytes("UTF-8"), 4, lo, b.getLocalPort()));
                java.net.DatagramPacket got = new java.net.DatagramPacket(buf, buf.length);
                b.receive(got);
                String first = new String(got.getData(), 0, got.getLength(), "UTF-8");
                b.send(new java.net.DatagramPacket("pong".getBytes("UTF-8"), 4, got.getAddress(), got.getPort()));
                java.net.DatagramPacket back = new java.net.DatagramPacket(new byte[64], 64);
                a.receive(back);
                String second = new String(back.getData(), 0, back.getLength(), "UTF-8");
                detail = first + " from " + got.getAddress() + ":" + got.getPort() + ", " + second;
                return "ping".equals(first) && got.getPort() == a.getLocalPort() && got.getAddress().isLoopbackAddress() && "pong".equals(second);
            }
        }));
        step("net-close", () -> offMain(() -> {
            /* A thread waits in accept; the socket is closed under it. libcore ends that wait with a signal to the thread, and accept throws. */
            final java.net.ServerSocket server = new java.net.ServerSocket(0, 5, java.net.InetAddress.getByName("127.0.0.1"));
            final String[] ended = new String[1];
            Thread t = new Thread(() -> {
                try {
                    server.accept().close();
                    ended[0] = "accepted";
                } catch (java.io.IOException e) {
                    ended[0] = e.getClass().getName() + ": " + e.getMessage();
                }
            }, "net-waiter");
            t.start();
            Thread.sleep(300);
            long began = System.nanoTime();
            server.close();
            t.join(5000);
            long ms = (System.nanoTime() - began) / 1000000;
            detail = ended[0] + " after " + ms + " ms";
            return !t.isAlive() && "java.net.SocketException: Socket closed".equals(ended[0]) && ms < 3000;
        }));
        step("net-nio", () -> offMain(() -> {
            /* java.nio: a selector over a server channel and a client channel, neither blocking; first woken from another thread (its wake-up is a pipe, waited on with the sockets). */
            try (java.nio.channels.Selector sel = java.nio.channels.Selector.open();
                 java.nio.channels.ServerSocketChannel server = java.nio.channels.ServerSocketChannel.open();
                 java.nio.channels.SocketChannel client = java.nio.channels.SocketChannel.open()) {
                server.bind(new java.net.InetSocketAddress(java.net.InetAddress.getByName("127.0.0.1"), 0));
                server.configureBlocking(false);
                server.register(sel, java.nio.channels.SelectionKey.OP_ACCEPT);
                new Thread(() -> {
                    try {
                        Thread.sleep(200);
                    } catch (InterruptedException e) {
                    }
                    sel.wakeup();
                }, "net-wakeup").start();
                long began = System.nanoTime();
                int woke = sel.select(5000);
                long wokeMs = (System.nanoTime() - began) / 1000000;
                client.configureBlocking(false);
                boolean at_once = client.connect(server.getLocalAddress());
                client.register(sel, at_once ? java.nio.channels.SelectionKey.OP_WRITE : java.nio.channels.SelectionKey.OP_CONNECT);
                java.nio.channels.SocketChannel accepted = null;
                java.nio.ByteBuffer in = java.nio.ByteBuffer.allocate(16);
                String got = null;
                long end = System.currentTimeMillis() + 10000;
                while (got == null && System.currentTimeMillis() < end) {
                    if (sel.select(1000) == 0) continue;
                    for (java.nio.channels.SelectionKey k : sel.selectedKeys()) {
                        if (!k.isValid()) continue;
                        if (k.isAcceptable()) {
                            accepted = server.accept();
                            accepted.configureBlocking(false);
                            accepted.register(sel, java.nio.channels.SelectionKey.OP_READ);
                        } else if (k.isConnectable()) {
                            if (client.finishConnect()) k.interestOps(java.nio.channels.SelectionKey.OP_WRITE);
                        } else if (k.isWritable()) {
                            client.write(java.nio.ByteBuffer.wrap("nio!".getBytes("UTF-8")));
                            k.interestOps(0);
                        } else if (k.isReadable()) {
                            ((java.nio.channels.SocketChannel) k.channel()).read(in);
                            if (in.position() >= 4) got = new String(in.array(), 0, in.position(), "UTF-8");
                        }
                    }
                    sel.selectedKeys().clear();
                }
                if (accepted != null) accepted.close();
                detail = "woken=" + woke + " after " + wokeMs + " ms, got=" + got;
                return woke == 0 && wokeMs < 3000 && "nio!".equals(got);
            }
        }));
        step("net-interfaces", () -> offMain(() -> {
            java.net.NetworkInterface lo = java.net.NetworkInterface.getByName("lo");
            java.util.List<String> names = new java.util.ArrayList<>();
            for (java.net.NetworkInterface n : java.util.Collections.list(java.net.NetworkInterface.getNetworkInterfaces())) names.add(n.getName());
            boolean v4 = false;
            if (lo != null)
                for (java.net.InetAddress a : java.util.Collections.list(lo.getInetAddresses())) v4 |= "127.0.0.1".equals(a.getHostAddress());
            detail = names + " lo=" + lo;
            return lo != null && names.contains("lo") && lo.isLoopback() && lo.isUp() && lo.getIndex() > 0 && v4
                    && lo.equals(java.net.NetworkInterface.getByIndex(lo.getIndex()));
        }));
        step("net-connectivity", () -> {
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            android.net.NetworkInfo info = cm.getActiveNetworkInfo();
            detail = info == null ? "no active network" : info.getTypeName() + " connected=" + info.isConnected();
            /* What the framework says: connected, over Wi-Fi. */
            return info != null && info.isConnected() && info.getType() == android.net.ConnectivityManager.TYPE_WIFI && cm.getActiveNetwork() != null;
        });
    }

    private static String describe(android.content.Intent battery) {
        return "level=" + battery.getIntExtra("level", -1) + "/" + battery.getIntExtra("scale", -1) + " status=" + battery.getIntExtra("status", -1) + " plugged="
                + battery.getIntExtra("plugged", -1) + " health=" + battery.getIntExtra("health", -1) + " voltage=" + battery.getIntExtra("voltage", -1) + " temperature="
                + battery.getIntExtra("temperature", -1) + " present=" + battery.getBooleanExtra("present", false) + " technology=" + battery.getStringExtra("technology");
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        boolean all = request == 77 && permissions.length == 2 && results.length == 2;
        for (int r : results) all &= r == android.content.pm.PackageManager.PERMISSION_GRANTED;
        /* Delivered as Android delivers it: later, on the main thread, not from inside requestPermissions. */
        Log.i("DataTest", "permission-result " + (all && android.os.Looper.myLooper() == android.os.Looper.getMainLooper() ? "ok" : "FAILED") + " after done=" + doneLogged);
    }

    private boolean doneLogged, insetsLogged;

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DataTest", "onDestroy");
    }
}
