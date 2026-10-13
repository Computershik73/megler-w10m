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
        setContentView(text);
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
        Log.i("DataTest", "done ok=" + ok + " failed=" + failed);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DataTest", "onDestroy");
    }
}
