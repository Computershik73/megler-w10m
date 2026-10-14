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

    /**
     * TLS while there is none: the contexts an app and Android's own HTTP client ask for are there, and a connection that would
     * use one fails as a connection fails -- an IOException -- not with the AssertionError Android's client throws when there is
     * no SSLContext at all, which ended threads of MX Player and Super S22 Launcher.
     */
    private void tls() {
        step("tls", () -> {
            javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
            context.init(null, null, null);
            javax.net.ssl.SSLSocketFactory factory = context.getSocketFactory();
            boolean there = factory != null && javax.net.ssl.SSLContext.getInstance("TLSv1.2") != null && javax.net.ssl.SSLContext.getDefault() != null
                    && javax.net.ssl.SSLSocketFactory.getDefault() != null && javax.net.ssl.HttpsURLConnection.getDefaultSSLSocketFactory() != null;
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
            detail = "there=" + there + " direct=" + direct + " https=" + https;
            return there && "no TLS on this phone yet".equals(direct) && "IOException".equals(https);
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

    private boolean doneLogged;

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DataTest", "onDestroy");
    }
}
