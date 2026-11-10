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
            // A call made by reflection fails as InvocationTargetException: what the method threw is its cause.
            if (t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null) detail += " from " + t.getCause();
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
        /*
         * The phone's bars: once the test says what they cover (megler_set_insets), the decor keeps the content off them, and
         * the content moves down by the top one and shrinks by both; then the app hides and shows them (bars()), and the test
         * hears what it asks for (megler_system_bars_host).
         */
        column.addOnLayoutChangeListener(new android.view.View.OnLayoutChangeListener() {
            int top = -1, height;
            boolean moved;

            @Override
            public void onLayoutChange(android.view.View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                int[] at = new int[2];
                v.getLocationOnScreen(at);
                if (top < 0) {
                    top = at[1];
                    height = v.getHeight();
                } else if (!moved && at[1] != top) {
                    moved = true;
                    Log.i("DataTest", "bars-insets moved=" + (at[1] - top) + " shrunk=" + (height - v.getHeight()));
                    bars(v);
                }
            }
        });
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
        step("wal-restored", () -> {
            /*
             * A database in the write-ahead log's format, as Android writes one and a backup brings one back (WhatsApp's
             * msgstore.db after a restore): made here in exclusive locking mode, the one way this SQLite writes that format,
             * left with its header saying so, then opened as an app opens it. Before 0.1.0.673 the first statement on it
             * failed with SQLITE_CANTOPEN; now the open makes it a rollback journal's and the rows are there.
             */
            deleteDatabase("restored.db");
            File file = getDatabasePath("restored.db");
            SQLiteDatabase w = SQLiteDatabase.openOrCreateDatabase(file, null);
            w.execSQL("PRAGMA locking_mode=EXCLUSIVE");
            String mode = DatabaseUtils.stringForQuery(w, "PRAGMA journal_mode=WAL", null);
            w.execSQL("CREATE TABLE m (id INTEGER PRIMARY KEY, body TEXT)");
            for (int i = 1; i <= 50; i++) w.execSQL("INSERT INTO m (body) VALUES (?)", new Object[] { "message " + i });
            w.close();
            byte[] head = new byte[20];
            try (java.io.FileInputStream in = new java.io.FileInputStream(file)) {
                if (in.read(head) != head.length) return false;
            }
            int before = head[18];
            SQLiteDatabase r = SQLiteDatabase.openDatabase(file.getPath(), null, SQLiteDatabase.OPEN_READWRITE);
            long rows = DatabaseUtils.longForQuery(r, "SELECT count(*) FROM m", null);
            String last = DatabaseUtils.stringForQuery(r, "SELECT body FROM m WHERE id = 50", null);
            r.close();
            try (java.io.FileInputStream in = new java.io.FileInputStream(file)) {
                if (in.read(head) != head.length) return false;
            }
            detail = "mode=" + mode + " header " + before + "->" + head[18] + " rows=" + rows + " last=" + last;
            return "wal".equals(mode) && before == 2 && head[18] == 1 && rows == 50 && "message 50".equals(last) && deleteDatabase("restored.db");
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
        phoneEvents();
        connectivity();
        dnsServers();
        systemLibs();
        apacheHttpLegacy();
        preloadedDrawables();
        location();
        sensors();
        timezone();
        locale();
        phoneNumber();
        tls();
        networkTrust();
        keystore(run);
        keystoreCertificates(run);
        keystoreForeignKey();
        pathOps();
        textPath();
        base64Streams();
        pipedStreams();
        codecs();
        userHandle();
        cleartext();
        compatChange();
        sharedLibrary();
        cameras();
        safetyCenter();
        parseIntent();
        xml();
        assetFd();
        tasks();
        wrapperReceiver();
        viewGroupFlag();
        textDrawables();
        pfdDup();
        pfdParcel();
        themeSelfRef();
        queueNext();
        storageStats();
        broadcastExplicit();
        serviceStopThread();
        // A stop by a start's id stops the service only when that was its latest start, as on Android (Twice says so).
        step("start-ids", () -> {
            startService(new android.content.Intent(this, Twice.class));
            startService(new android.content.Intent(this, Twice.class));
            return true;
        });
        // A start has its id when it is asked for: a service that starts itself again and then stops by its first start's id
        // goes on, in the same instance, as GmsCore's IntentOperationService does (Again says so).
        // A service the manifest puts in another process runs in a class scope of its own (megler.Processes), with that process's
        // name and its own statics: this scope's sMarker is 7, the other's 0. Asked through transact, as across processes.
        step("process-scope", () -> {
            sMarker = 7;
            bindService(new android.content.Intent(this, Elsewhere.class), new android.content.ServiceConnection() {
                @Override public void onServiceConnected(android.content.ComponentName n, android.os.IBinder b) {
                    android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
                    String said;
                    try { b.transact(1, data, reply, 0); said = reply.readString(); } catch (android.os.RemoteException e) { said = String.valueOf(e); }
                    data.recycle();
                    reply.recycle();
                    Log.i("DataTest", "process-scope " + said + " here=" + Main.class.getClassLoader().getClass().getSimpleName().equals("ServiceClassLoader"));
                    // Then a broadcast from that process, its extra an object of that process's class: this process's receiver
                    // has it as one of its own class (megler.Processes.forLoader).
                    registerReceiver(new android.content.BroadcastReceiver() {
                        @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                            String got;
                            try { Note n = i.getParcelableExtra("note"); got = n != null ? n.text : "null"; } catch (ClassCastException e) { got = String.valueOf(e); }
                            Log.i("DataTest", "process-intent note=" + got);
                        }
                    }, new android.content.IntentFilter("megler.test.FROM_ELSEWHERE"));
                    android.os.Parcel d2 = android.os.Parcel.obtain();
                    try { b.transact(2, d2, null, 0); } catch (android.os.RemoteException e) { Log.w("DataTest", "process-intent: " + e); }
                    d2.recycle();
                    // And a bundle in its reply with a Note of its process's class (as Google Play services' sign-in answers
                    // with its TokenData): given this process's loader, the bundle has the Note as one of this process's class.
                    android.os.Parcel d3 = android.os.Parcel.obtain(), r3 = android.os.Parcel.obtain();
                    String inBundle;
                    try {
                        b.transact(3, d3, r3, 0);
                        r3.setDataPosition(0);
                        android.os.Bundle got = r3.readBundle();
                        got.setClassLoader(Main.class.getClassLoader());
                        android.os.Bundle inner = got.getBundle("details");
                        inner.setClassLoader(Main.class.getClassLoader());
                        Note kept = (Note) inner.getParcelable("note");
                        inBundle = kept != null ? kept.text : "null";
                    } catch (Exception e) { inBundle = String.valueOf(e); }
                    d3.recycle();
                    r3.recycle();
                    Log.i("DataTest", "process-bundle note=" + inBundle);
                }
                @Override public void onServiceDisconnected(android.content.ComponentName n) {}
            }, BIND_AUTO_CREATE);
            return true;
        });
        // Jobs are run (JobScheduler): one scheduled with extras and a minimum latency, which the scheduler tells back with its
        // service; one given work twice, which it takes in order.
        step("job-scheduler", () -> {
            android.app.job.JobScheduler js = (android.app.job.JobScheduler) getSystemService(JOB_SCHEDULER_SERVICE);
            android.os.PersistableBundle extras = new android.os.PersistableBundle();
            extras.putString("said", "hello");
            android.content.ComponentName work = new android.content.ComponentName(this, Work.class);
            int r = js.schedule(new android.app.job.JobInfo.Builder(41, work).setExtras(extras).setMinimumLatency(100)
                    .setRequiredNetworkType(android.app.job.JobInfo.NETWORK_TYPE_ANY).build());
            android.app.job.JobInfo pending = js.getPendingJob(41);
            android.app.job.JobInfo queue = new android.app.job.JobInfo.Builder(42, work).build();
            js.enqueue(queue, new android.app.job.JobWorkItem(new android.content.Intent("one")));
            js.enqueue(queue, new android.app.job.JobWorkItem(new android.content.Intent("two")));
            // And work for a service that is no JobService but has an engine, as AndroidX's JobIntentService has.
            android.app.job.JobInfo engined = new android.app.job.JobInfo.Builder(43, new android.content.ComponentName(this, EngineWork.class)).build();
            js.enqueue(engined, new android.app.job.JobWorkItem(new android.content.Intent("three")));
            js.enqueue(engined, new android.app.job.JobWorkItem(new android.content.Intent("four")));
            detail = "result=" + r + " pending=" + pending + " latency=" + (pending != null ? pending.getMinLatencyMillis() : -1);
            return r == android.app.job.JobScheduler.RESULT_SUCCESS && pending != null && Work.class.getName().equals(pending.getService().getClassName())
                && pending.getMinLatencyMillis() == 100;
        });
        // A download enqueued in the system's download provider (content://downloads, as DownloadManager and Play Store's old
        // downloader do), from a server of this app's own on 127.0.0.1 that wants the row's cookie: its cursor's observer hears
        // it go (ContentObservable, AbstractCursor.setNotificationUri), DOWNLOAD_COMPLETE comes with its id, and its row and its
        // file say what came.
        step("downloads", () -> {
            final java.net.ServerSocket server = new java.net.ServerSocket(0, 1, java.net.InetAddress.getByName("127.0.0.1"));
            final boolean[] cookie = { false };
            new Thread(() -> {
                try (java.net.Socket s = server.accept()) {
                    java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(s.getInputStream(), "ISO-8859-1"));
                    for (String line; (line = in.readLine()) != null && !line.isEmpty(); ) if (line.equalsIgnoreCase("Cookie: a=b")) cookie[0] = true;
                    s.getOutputStream().write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhello".getBytes("ISO-8859-1"));
                    s.getOutputStream().flush();
                } catch (java.io.IOException e) {
                    Log.w("DataTest", "downloads server: " + e);
                } finally {
                    try { server.close(); } catch (java.io.IOException e) {}
                }
            }, "downloads-server").start();
            final android.net.Uri all = android.net.Uri.parse("content://downloads/my_downloads");
            final android.database.Cursor watched = getContentResolver().query(all, null, null, null, null);
            final boolean[] observed = { false };
            final android.database.ContentObserver watcher = new android.database.ContentObserver(null) {
                @Override public void onChange(boolean self) { observed[0] = true; }
            };
            if (watched != null) watched.registerContentObserver(watcher);
            final java.io.File file = new java.io.File(getCacheDir(), "downloaded.txt");
            file.delete();
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    long id = i.getLongExtra("extra_download_id", -1);
                    String status = "?", bytes = "?", body = "?";
                    try (android.database.Cursor row = getContentResolver().query(android.net.Uri.withAppendedPath(all, Long.toString(id)), new String[] { "status", "current_bytes" }, null, null, null)) {
                        if (row != null && row.moveToFirst()) { status = row.getString(0); bytes = row.getString(1); }
                    }
                    try (java.io.InputStream in = getContentResolver().openInputStream(android.net.Uri.withAppendedPath(all, Long.toString(id)))) {
                        byte[] b = new byte[16];
                        int n = in.read(b);
                        body = n > 0 ? new String(b, 0, n) : "";
                    } catch (java.io.IOException e) { body = String.valueOf(e); }
                    if (watched != null) { watched.unregisterContentObserver(watcher); watched.close(); }
                    Log.i("DataTest", "downloads ok status=" + status + " bytes=" + bytes + " body=" + body + " cookie=" + cookie[0] + " observed=" + observed[0]
                            + " file=" + file.length());
                }
            }, new android.content.IntentFilter("android.intent.action.DOWNLOAD_COMPLETE"));
            android.content.ContentValues v = new android.content.ContentValues();
            v.put("uri", "http://127.0.0.1:" + server.getLocalPort() + "/hello.txt");
            v.put("destination", 4);
            v.put("hint", android.net.Uri.fromFile(file).toString());
            v.put("cookiedata", "a=b");
            v.put("notificationpackage", getPackageName());
            android.net.Uri row = getContentResolver().insert(all, v);
            detail = "row=" + row;
            return row != null && row.toString().startsWith("content://downloads/my_downloads/");
        });
        // Android 10's rollback service (a system API, so by reflection): there, with no rollbacks (Play Store ends without it).
        step("rollback-service", () -> {
            Object rollbacks = getSystemService("rollback");
            if (rollbacks == null) { detail = "none"; return false; }
            java.util.List<?> available = (java.util.List<?>) rollbacks.getClass().getMethod("getAvailableRollbacks").invoke(rollbacks);
            java.util.List<?> committed = (java.util.List<?>) rollbacks.getClass().getMethod("getRecentlyCommittedRollbacks").invoke(rollbacks);
            detail = rollbacks.getClass().getName() + " available=" + available + " committed=" + committed;
            return available.isEmpty() && committed.isEmpty();
        });
        // The ART manager (PackageManager.getArtManager, a system API, so by reflection): there, with profiling off.
        step("art-manager", () -> {
            Object art = getPackageManager().getClass().getMethod("getArtManager").invoke(getPackageManager());
            if (art == null) { detail = "none"; return false; }
            Object on = art.getClass().getMethod("isRuntimeProfilingEnabled", int.class).invoke(art, 0);
            detail = art.getClass().getName() + " profiling=" + on;
            return Boolean.FALSE.equals(on);
        });
        // A ProgressBar that overrides getIndeterminateDrawable to cast it to a drawable of its own, as Material's progress
        // indicators do, is made, and its indeterminate drawable is none until it sets one; a plain one has one to colour.
        step("progress-subclass", () -> {
            Indicator i = new Indicator(this);
            android.graphics.drawable.Drawable plain = new android.widget.ProgressBar(this).getIndeterminateDrawable();
            detail = "indicator=" + i.getIndeterminateDrawable() + " plain=" + plain;
            return i.getIndeterminateDrawable() == null && plain != null;
        });
        // A phone with its SIM, as the shell said it (megler_set_cellular): GSM, ready, the operator, its country, LTE; and
        // the telephony and microphone features. The harness gives a SIM of the run's region: 310260 in the US, 25001 in
        // Russia.
        step("telephony", () -> {
            android.telephony.TelephonyManager tm = (android.telephony.TelephonyManager) getSystemService(TELEPHONY_SERVICE);
            android.content.pm.PackageManager pm = getPackageManager();
            boolean russia = "RU".equals(java.util.Locale.getDefault().getCountry());
            String operator = russia ? "25001" : "310260", iso = russia ? "ru" : "us";
            detail = "type=" + tm.getPhoneType() + " sim=" + tm.getSimState() + " op=" + tm.getSimOperator() + "/" + tm.getSimOperatorName()
                    + " net=" + tm.getNetworkOperator() + " iso=" + tm.getSimCountryIso() + " nt=" + tm.getNetworkType();
            return tm.getPhoneType() == android.telephony.TelephonyManager.PHONE_TYPE_GSM && tm.getSimState() == android.telephony.TelephonyManager.SIM_STATE_READY
                && operator.equals(tm.getSimOperator()) && "Test Operator".equals(tm.getSimOperatorName()) && operator.equals(tm.getNetworkOperator())
                && iso.equals(tm.getSimCountryIso()) && iso.equals(tm.getNetworkCountryIso()) && tm.getNetworkType() == 13 && !tm.isNetworkRoaming()
                && pm.hasSystemFeature(android.content.pm.PackageManager.FEATURE_TELEPHONY) && pm.hasSystemFeature(android.content.pm.PackageManager.FEATURE_MICROPHONE);
        });
        // What the microphone hears, through AudioRecord: the harness's ramp (mic_start), 16-bit and as floats.
        step("microphone", () -> {
            android.media.AudioRecord r = new android.media.AudioRecord(android.media.MediaRecorder.AudioSource.MIC, 16000,
                    android.media.AudioFormat.CHANNEL_IN_MONO, android.media.AudioFormat.ENCODING_PCM_16BIT, 6400);
            r.startRecording();
            short[] s = new short[800];
            int got = r.read(s, 0, s.length);
            boolean ramp = got == s.length;
            for (int i = 1; i < got; i++) if ((short) (s[i] - s[i - 1]) != 1 && s[i] != 0) ramp = false;
            r.stop();
            r.release();
            detail = "got=" + got + " first=" + (got > 0 ? s[0] : -1) + " last=" + (got > 0 ? s[got - 1] : -1);
            return ramp && s[got - 1] != 0;
        });
        // The phone's check of its user (megler.Biometrics, the harness's bio_verify), as Play Store's fingerprint sign-in
        // asks it: BiometricManager and FingerprintManager say the phone has it; a prompt with a CryptoObject succeeds and
        // gives that object back; a prompt the user says no to calls its negative button's listener.
        step("biometric", () -> {
            android.hardware.biometrics.BiometricManager bm = getSystemService(android.hardware.biometrics.BiometricManager.class);
            android.hardware.fingerprint.FingerprintManager fm = (android.hardware.fingerprint.FingerprintManager) getSystemService(FINGERPRINT_SERVICE);
            boolean can = bm != null && bm.canAuthenticate() == android.hardware.biometrics.BiometricManager.BIOMETRIC_SUCCESS
                && fm != null && fm.isHardwareDetected() && fm.hasEnrolledFingerprints()
                && getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_FINGERPRINT);
            java.security.Signature signature = java.security.Signature.getInstance("SHA256withRSA");
            android.hardware.biometrics.BiometricPrompt.CryptoObject crypto = new android.hardware.biometrics.BiometricPrompt.CryptoObject(signature);
            final java.util.concurrent.CountDownLatch first = new java.util.concurrent.CountDownLatch(1), second = new java.util.concurrent.CountDownLatch(1);
            final Object[] got = new Object[2];
            java.util.concurrent.Executor now = Runnable::run;
            new android.hardware.biometrics.BiometricPrompt.Builder(this).setTitle("Sign in").setDescription("to the test")
                .setNegativeButton("Cancel", now, (d, which) -> { got[0] = "negative"; first.countDown(); }).build()
                .authenticate(crypto, new android.os.CancellationSignal(), now, new android.hardware.biometrics.BiometricPrompt.AuthenticationCallback() {
                    @Override public void onAuthenticationSucceeded(android.hardware.biometrics.BiometricPrompt.AuthenticationResult r) { got[0] = r.getCryptoObject(); first.countDown(); }
                    @Override public void onAuthenticationError(int code, CharSequence why) { got[0] = "error " + code; first.countDown(); }
                });
            boolean answered = first.await(10, java.util.concurrent.TimeUnit.SECONDS);
            new android.hardware.biometrics.BiometricPrompt.Builder(this).setTitle("Say no")
                .setNegativeButton("Cancel", now, (d, which) -> { got[1] = "negative " + which; second.countDown(); }).build()
                .authenticate(new android.os.CancellationSignal(), now, new android.hardware.biometrics.BiometricPrompt.AuthenticationCallback() {
                    @Override public void onAuthenticationSucceeded(android.hardware.biometrics.BiometricPrompt.AuthenticationResult r) { got[1] = "succeeded"; second.countDown(); }
                    @Override public void onAuthenticationError(int code, CharSequence why) { got[1] = "error " + code; second.countDown(); }
                });
            answered &= second.await(10, java.util.concurrent.TimeUnit.SECONDS);
            boolean same = got[0] instanceof android.hardware.biometrics.BiometricPrompt.CryptoObject
                && ((android.hardware.biometrics.BiometricPrompt.CryptoObject) got[0]).getSignature() == signature;
            detail = "can=" + can + " first=" + (same ? "crypto" : got[0]) + " second=" + got[1];
            return can && answered && same && ("negative " + android.content.DialogInterface.BUTTON_NEGATIVE).equals(got[1]);
        });
        // An ordered broadcast with a result receiver, as Play Store sends its own receivers one when it takes an install up:
        // the receiver of the higher priority first, its result passed to the next, which goes asynchronous and adds to it
        // from another thread, then the result receiver, on its handler's thread, with the result as it ended.
        step("ordered-broadcast", () -> {
            android.os.HandlerThread ht = new android.os.HandlerThread("ordered");
            ht.start();
            android.os.Handler h = new android.os.Handler(ht.getLooper());
            final String action = "megler.test.data.ORDERED";
            final StringBuilder order = new StringBuilder();
            final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
            final Object[] got = new Object[4];
            android.content.BroadcastReceiver low = new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    order.append("low;");
                    final PendingResult p = goAsync();
                    final boolean ordered = isOrderedBroadcast();
                    new Thread(() -> {
                        p.setResultData(p.getResultData() + "b");
                        p.getResultExtras(true).putBoolean("ordered", ordered);
                        p.finish();
                    }).start();
                }
            };
            android.content.BroadcastReceiver high = new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    order.append("high;");
                    setResultCode(getResultCode() + 1);
                    setResultData(getResultData() + "a");
                }
            };
            android.content.IntentFilter fl = new android.content.IntentFilter(action), fh = new android.content.IntentFilter(action);
            fh.setPriority(10);
            registerReceiver(low, fl, null, h);
            registerReceiver(high, fh, null, h);
            sendOrderedBroadcast(new android.content.Intent(action), null, new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    got[0] = getResultCode();
                    got[1] = getResultData();
                    android.os.Bundle e = getResultExtras(false);
                    got[2] = e != null && e.getBoolean("ordered");
                    got[3] = android.os.Looper.myLooper() == ht.getLooper();
                    done.countDown();
                }
            }, h, 41, "", null);
            boolean answered = done.await(10, java.util.concurrent.TimeUnit.SECONDS);
            unregisterReceiver(low);
            unregisterReceiver(high);
            ht.quitSafely();
            detail = "order=" + order + " code=" + got[0] + " data=" + got[1] + " ordered=" + got[2] + " onHandler=" + got[3];
            return answered && "high;low;".equals(order.toString()) && Integer.valueOf(42).equals(got[0]) && "ab".equals(got[1])
                && Boolean.TRUE.equals(got[2]) && Boolean.TRUE.equals(got[3]);
        });
        // Views attached to the window are hardware accelerated, as Android's are, and draw on a canvas that says so (their
        // display list's), as Compose asks before it keeps its graphics layers as RenderNodes; a SurfaceView, which has no
        // list of its own, draws on one that does not say so, as a WebView does; a view not attached, and a bitmap's canvas,
        // are not.
        step("hardware-accelerated", () -> {
            final boolean[] seen = new boolean[3];
            android.widget.FrameLayout group = new android.widget.FrameLayout(this);
            android.view.View plain = new android.view.View(this) {
                @Override protected void onDraw(android.graphics.Canvas c) { seen[0] = true; seen[1] = c.isHardwareAccelerated(); }
            };
            android.view.SurfaceView surface = new android.view.SurfaceView(this) {
                @Override public void draw(android.graphics.Canvas c) { seen[2] = c.isHardwareAccelerated(); super.draw(c); }
            };
            boolean detached = plain.isHardwareAccelerated();
            group.addView(plain, new android.widget.FrameLayout.LayoutParams(40, 40));
            group.addView(surface, new android.widget.FrameLayout.LayoutParams(40, 40));
            ((android.view.ViewGroup) text.getParent()).addView(group, new android.view.ViewGroup.LayoutParams(80, 80));
            // (As on Android, a view is hardware accelerated once its window is attached: here the steps run before it is.)
            boolean window = text.isAttachedToWindow();
            boolean attached = plain.isHardwareAccelerated(), surfaceAttached = surface.isHardwareAccelerated();
            group.measure(android.view.View.MeasureSpec.makeMeasureSpec(80, android.view.View.MeasureSpec.EXACTLY), android.view.View.MeasureSpec.makeMeasureSpec(80, android.view.View.MeasureSpec.EXACTLY));
            group.layout(0, 0, 80, 80);
            seen[2] = true;
            // (Drawn into a list as its parent's list would draw it: the SurfaceView draws into that list's canvas itself.)
            android.graphics.RenderNode node = new android.graphics.RenderNode("hardware-accelerated");
            node.setPosition(0, 0, 80, 80);
            android.graphics.RecordingCanvas rc = node.beginRecording();
            group.draw(rc);
            node.endRecording();
            android.graphics.Canvas bc = new android.graphics.Canvas(android.graphics.Bitmap.createBitmap(8, 8, android.graphics.Bitmap.Config.ARGB_8888));
            ((android.view.ViewGroup) group.getParent()).removeView(group);
            detail = "detached=" + detached + " attached=" + attached + " window=" + window + " drawn=" + seen[0] + " canvas=" + seen[1] + " surface=" + surfaceAttached + "/" + seen[2]
                + " bitmap=" + bc.isHardwareAccelerated();
            return !detached && attached == window && seen[0] && seen[1] && !surfaceAttached && !seen[2] && !bc.isHardwareAccelerated() && rc.isHardwareAccelerated();
        });
        // A view's invalidation reaches the nearest parent with an onDescendantInvalidated of its own, as Android's
        // hardware-accelerated invalidation goes up through it, with the child that leads to the view: Compose's
        // AndroidViewHolder records its layer again so, and a SurfaceView in an AndroidView (RUTUBE's player) was drawn once
        // and never again. A parent of the framework's own in between is passed; told once, as the holder's super does not
        // tell it again.
        step("descendant-invalidated", () -> {
            final android.view.View[] told = new android.view.View[2];
            final int[] calls = new int[1];
            android.widget.FrameLayout holder = new android.widget.FrameLayout(this) {
                @Override public void onDescendantInvalidated(android.view.View child, android.view.View target) {
                    calls[0]++;
                    told[0] = child;
                    told[1] = target;
                    super.onDescendantInvalidated(child, target);
                }
            };
            android.widget.FrameLayout middle = new android.widget.FrameLayout(this);
            android.view.View leaf = new android.view.View(this);
            middle.addView(leaf, new android.widget.FrameLayout.LayoutParams(10, 10));
            holder.addView(middle, new android.widget.FrameLayout.LayoutParams(20, 20));
            ((android.view.ViewGroup) text.getParent()).addView(holder, new android.view.ViewGroup.LayoutParams(20, 20));
            calls[0] = 0;
            told[0] = told[1] = null;
            leaf.invalidate();
            int afterLeaf = calls[0];
            boolean child = told[0] == middle, target = told[1] == leaf;
            ((android.view.ViewGroup) holder.getParent()).removeView(holder);
            detail = "calls=" + afterLeaf + " child=" + child + " target=" + target;
            return afterLeaf == 1 && child && target;
        });
        // An image matrix set before the view is laid out, then the scale type made MATRIX, as Material's FloatingActionButton
        // does as it is made: once laid out, the view draws by that matrix.
        step("image-matrix-before-layout", () -> {
            android.widget.ImageView iv = new android.widget.ImageView(this);
            iv.setImageDrawable(new android.graphics.drawable.BitmapDrawable(getResources(), android.graphics.Bitmap.createBitmap(10, 10, android.graphics.Bitmap.Config.ARGB_8888)));
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setScale(2f, 2f);
            iv.setImageMatrix(m);
            iv.setScaleType(android.widget.ImageView.ScaleType.MATRIX);
            iv.measure(android.view.View.MeasureSpec.makeMeasureSpec(100, android.view.View.MeasureSpec.EXACTLY), android.view.View.MeasureSpec.makeMeasureSpec(100, android.view.View.MeasureSpec.EXACTLY));
            iv.layout(0, 0, 100, 100);
            float[] v = new float[9];
            iv.getImageMatrix().getValues(v);
            detail = java.util.Arrays.toString(v);
            return v[0] == 2f && v[4] == 2f && v[2] == 0f && v[5] == 0f;
        });
        // A DelegateLastClassLoader over a DEX of a module's own (assets/module.dex) has its classes before its parent's: its
        // Twin, of the same name as the app's, is its own, and so is the Twin its ModuleProbe calls, as a module's own
        // classes of Google Play services are (dalvik.system's 4-argument constructor is Android 10's). A PathClassLoader
        // over the same file, whose parent is the app's, still gives the app's.
        step("delegate-last-scope", () -> {
            java.io.File f = new java.io.File(getCacheDir(), "module.dex");
            try (java.io.InputStream in = getAssets().open("module.dex"); java.io.OutputStream out = new java.io.FileOutputStream(f)) {
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
            }
            ClassLoader app = getClassLoader();
            dalvik.system.DelegateLastClassLoader last = new dalvik.system.DelegateLastClassLoader(f.getPath(), null, app, false);
            Class<?> twin = last.loadClass("megler.test.data.Twin");
            String own = (String) twin.getMethod("who").invoke(null);
            String inner = (String) last.loadClass("megler.test.data.ModuleProbe").getMethod("twin").invoke(null);
            String plain = (String) new dalvik.system.PathClassLoader(f.getPath(), app).loadClass("megler.test.data.Twin").getMethod("who").invoke(null);
            detail = "own=" + own + " inner=" + inner + " app=" + Twin.who() + " plain=" + plain + " loader=" + (twin.getClassLoader() == last);
            return "module".equals(own) && "module".equals(inner) && "app".equals(Twin.who()) && "app".equals(plain) && twin != Twin.class && twin.getClassLoader() == last;
        });
        // An APK's manifest read through an AssetManager of one's own, as Google Play services 26 reads each of its modules':
        // the hidden constructor, addAssetPath, openXmlResourceParser on the cookie it gave.
        step("asset-manager-manifest", () -> {
            android.content.res.AssetManager am = android.content.res.AssetManager.class.getDeclaredConstructor().newInstance();
            int cookie = (Integer) android.content.res.AssetManager.class.getMethod("addAssetPath", String.class).invoke(am, getApplicationInfo().sourceDir);
            android.content.res.XmlResourceParser p = am.openXmlResourceParser(cookie, "AndroidManifest.xml");
            String pkg = null;
            for (int ev = p.getEventType(); ev != org.xmlpull.v1.XmlPullParser.END_DOCUMENT && pkg == null; ev = p.next())
                if (ev == org.xmlpull.v1.XmlPullParser.START_TAG && "manifest".equals(p.getName())) pkg = p.getAttributeValue(null, "package");
            p.close();
            detail = "cookie=" + cookie + " package=" + pkg;
            return cookie != 0 && getPackageName().equals(pkg);
        });
        // A large asset the APK deflates, read through AssetManager.open as it is inflated (not whole into an array first):
        // every byte as made, available() what is left, and the bytes after a mark read again after reset().
        step("big-asset", () -> {
            int size = 3 << 20, bad = -1, at = 0;
            byte[] buf = new byte[65536];
            try (java.io.InputStream in = getAssets().open("big-deflated.bin")) {
                boolean avail = in.available() == size;
                for (int r; (r = in.read(buf)) > 0; at += r)
                    for (int k = 0; k < r && bad < 0; k++)
                        if (buf[k] != (byte) ((at + k) * 31 + ((at + k) >> 12))) bad = at + k;
                detail = "class=" + in.getClass().getSimpleName() + " read=" + at + " bad=" + bad + " available=" + avail;
                if (!avail || at != size || bad >= 0) return false;
            }
            try (java.io.InputStream in = getAssets().open("big-deflated.bin")) {
                byte[] one = new byte[1000], two = new byte[1000];
                in.skip(1000000);
                in.mark(1 << 20);
                int a = in.read(one), b;
                in.reset();
                b = in.read(two);
                detail += " again=" + (a == 1000 && b == 1000 && java.util.Arrays.equals(one, two) && one[0] == (byte) (1000000 * 31 + (1000000 >> 12)));
                return a == 1000 && b == 1000 && java.util.Arrays.equals(one, two) && one[0] == (byte) (1000000 * 31 + (1000000 >> 12))
                        && in.getClass().getSimpleName().equals("InflatedAssetStream");
            }
        });
        // A path's segments through Path.getPathIterator, as Compose walks them on Android 14 (the Play Store's app pages): a
        // line, quad or cubic with its start point first, a rectangle in lines, a close without points, and then done. By
        // reflection, since the app is built against Android 10's android.jar.
        step("path-iterator", () -> {
            android.graphics.Path p = new android.graphics.Path();
            p.moveTo(10, 20);
            p.lineTo(30, 20);
            p.quadTo(40, 30, 30, 40);
            p.cubicTo(20, 50, 10, 50, 10, 40);
            p.close();
            p.addRect(0, 0, 5, 5, android.graphics.Path.Direction.CW);
            java.lang.reflect.Method iterator = android.graphics.Path.class.getMethod("getPathIterator");
            Class<?> ic = Class.forName("android.graphics.PathIterator"), sc = Class.forName("android.graphics.PathIterator$Segment");
            java.lang.reflect.Method hasNext = ic.getMethod("hasNext"), next = ic.getMethod("next", float[].class, int.class), peek = ic.getMethod("peek"),
                    segment = ic.getMethod("next"), verbOf = sc.getMethod("getVerb"), pointsOf = sc.getMethod("getPoints");
            Object it = iterator.invoke(p);
            StringBuilder verbs = new StringBuilder();
            float[] pts = new float[10];
            boolean points = true;
            while ((Boolean) hasNext.invoke(it)) {
                verbs.append((int) (Integer) next.invoke(it, pts, 2));
                if (verbs.length() == 2) points &= pts[2] == 10 && pts[3] == 20 && pts[4] == 30 && pts[5] == 20;
                if (verbs.length() == 4) points &= pts[2] == 30 && pts[3] == 40 && pts[4] == 20 && pts[5] == 50 && pts[8] == 10 && pts[9] == 40;
                if (verbs.length() == 8) points &= pts[2] == 5 && pts[3] == 0 && pts[4] == 5 && pts[5] == 5;
            }
            boolean done = (Integer) peek.invoke(it) == 6 && (Integer) next.invoke(it, pts, 0) == 6;
            android.graphics.Path moved = new android.graphics.Path(p);
            moved.offset(1, 1);
            Object first = segment.invoke(iterator.invoke(moved));
            float[] at = (float[]) pointsOf.invoke(first);
            boolean offset = (Integer) verbOf.invoke(first) == 0 && at[0] == 11 && at[1] == 21;
            boolean empty = !(Boolean) hasNext.invoke(iterator.invoke(new android.graphics.Path())), small = false;
            try {
                next.invoke(iterator.invoke(p), new float[9], 2);
            } catch (java.lang.reflect.InvocationTargetException e) {
                small = e.getCause() instanceof ArrayIndexOutOfBoundsException;
            }
            detail = "verbs=" + verbs + " points=" + points + " done=" + done + " offset=" + offset + " empty=" + empty + " small=" + small;
            return verbs.toString().equals("0124501115") && points && done && offset && empty && small;
        });
        // A MutableContextWrapper's new base, as WhatsApp's prewarmed chat views get the Conversation activity: the wrapper then
        // answers as that context does, and unwrapping it leads to the activity.
        step("mutable-context", () -> {
            android.content.MutableContextWrapper w = new android.content.MutableContextWrapper(getApplicationContext());
            android.view.View v = new android.view.View(w);
            w.setBaseContext(this);
            android.content.Context c = v.getContext();
            while (c instanceof android.content.ContextWrapper && !(c instanceof android.app.Activity)) c = ((android.content.ContextWrapper) c).getBaseContext();
            boolean based = w.getBaseContext() == this, found = c == this, theme = w.getTheme() == getTheme();
            detail = "based=" + based + " found=" + found + " theme=" + theme;
            return based && found && theme;
        });
        // Runtime.exec of a program the phone does not have: an IOException each time, as Android says it of a missing program,
        // and never an Error from the process class's initialisation (Facebook ended so at its first exec).
        step("exec", () -> {
            int refused = 0;
            String said = null;
            for (int i = 0; i < 2; i++) {
                try {
                    Runtime.getRuntime().exec(new String[] { "getprop", "ro.build.version.sdk" });
                } catch (java.io.IOException e) {
                    refused++;
                    said = e.getMessage();
                }
            }
            detail = "refused=" + refused + " said=" + said;
            return refused == 2 && said != null && said.contains("error=2");
        });
        // CRC-32C, which Android 14 has (java.util.zip.CRC32C; Yandex Maps checks its saved routes with it): the standard check
        // value of "123456789", the same through Checksum.update(byte[]) and update(ByteBuffer), the interface's defaults, and
        // after reset. By reflection: the app is built against Android 10's android.jar.
        step("crc32c", () -> {
            byte[] digits = "123456789".getBytes("US-ASCII");
            Class<?> c = Class.forName("java.util.zip.CRC32C");
            java.util.zip.Checksum one = (java.util.zip.Checksum) c.newInstance();
            one.update(digits, 0, digits.length);
            java.util.zip.Checksum two = (java.util.zip.Checksum) c.newInstance();
            java.util.zip.Checksum.class.getMethod("update", byte[].class).invoke(two, (Object) digits);
            java.util.zip.Checksum three = (java.util.zip.Checksum) c.newInstance();
            java.nio.ByteBuffer direct = java.nio.ByteBuffer.allocateDirect(digits.length);
            direct.put(digits).flip();
            java.util.zip.Checksum.class.getMethod("update", java.nio.ByteBuffer.class).invoke(three, direct);
            long v = one.getValue();
            one.reset();
            one.update('1');
            java.util.zip.Adler32 adler = new java.util.zip.Adler32();
            java.util.zip.Checksum.class.getMethod("update", byte[].class).invoke(adler, (Object) digits);
            detail = "crc32c=" + Long.toHexString(v) + " interface=" + Long.toHexString(two.getValue()) + " buffer=" + Long.toHexString(three.getValue())
                     + " one=" + Long.toHexString(one.getValue()) + " adler=" + Long.toHexString(adler.getValue()) + " consumed=" + !direct.hasRemaining();
            return v == 0xE3069283L && two.getValue() == v && three.getValue() == v && one.getValue() == 0x90F599E3L
                   && adler.getValue() == 0x091E01DEL && !direct.hasRemaining();
        });
        // OpenJDK 11's comparisons of arrays and of their ranges, which Android 13 has (java.util.Arrays.equals(byte[], int, int,
        // byte[], int, int) and the rest, the libcore overlay's): Aliucord Manager's patching of Discord's APK called that one
        // and ended on a NoSuchMethodError. By reflection: the app is built against Android 10's android.jar.
        // IntentFilter.matchCategories as Android has it: null when every category of the intent's is the filter's, else the first
        // that is not (it said null for anything; megler.MicroG matches Google Play services' services with it).
        // SubscriptionManager's lists of active subscription ids, never null as on Android (Android 11's methods: by reflection)
        // -- Google Play services' check-in took their length and ended on a NullPointerException, and Play Store had no
        // Android ID.
        // NinePatchDrawable's constructors without Resources, and with Resources and a NinePatch, which Android has: Telegram
        // builds its message bubbles with the first (in a try that keeps quiet), and drew no bubble and no text without it.
        step("nine-patch-constructors", () -> {
            android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(9, 9, android.graphics.Bitmap.Config.ARGB_8888);
            b.eraseColor(0xff00ff00);
            java.nio.ByteBuffer c = java.nio.ByteBuffer.allocate(32 + 4 * 4 + 9 * 4).order(java.nio.ByteOrder.nativeOrder());
            c.put((byte) 1).put((byte) 2).put((byte) 2).put((byte) 9).putInt(0).putInt(0);
            c.putInt(1).putInt(2).putInt(3).putInt(4).putInt(0);
            c.putInt(3).putInt(6).putInt(3).putInt(6);
            for (int i = 0; i < 9; i++) c.putInt(1);
            byte[] chunk = c.array();
            android.graphics.drawable.NinePatchDrawable bubble = new android.graphics.drawable.NinePatchDrawable(b, chunk, new android.graphics.Rect(), null);
            android.graphics.drawable.NinePatchDrawable other = new android.graphics.drawable.NinePatchDrawable(getResources(), new android.graphics.NinePatch(b, chunk, null));
            android.graphics.Bitmap out = android.graphics.Bitmap.createBitmap(30, 20, android.graphics.Bitmap.Config.ARGB_8888);
            bubble.setBounds(0, 0, 30, 20);
            bubble.setAlpha(200);
            bubble.draw(new android.graphics.Canvas(out));
            detail = "width=" + bubble.getIntrinsicWidth() + " alpha=" + bubble.getAlpha() + " other=" + other.getIntrinsicHeight();
            return bubble.getIntrinsicWidth() == 9 && bubble.getAlpha() == 200 && other.getIntrinsicHeight() == 9;
        });
        step("subscription-ids", () -> {
            android.telephony.SubscriptionManager sm = (android.telephony.SubscriptionManager) getSystemService(TELEPHONY_SUBSCRIPTION_SERVICE);
            Object active = android.telephony.SubscriptionManager.class.getMethod("getActiveSubscriptionIdList").invoke(sm);
            Object complete = android.telephony.SubscriptionManager.class.getMethod("getCompleteActiveSubscriptionIdList").invoke(sm);
            detail = "active=" + (active == null ? "null" : ((int[]) active).length) + " complete=" + (complete == null ? "null" : ((int[]) complete).length)
                    + " infos=" + sm.getActiveSubscriptionInfoCount();
            return active instanceof int[] && complete instanceof int[] && ((int[]) active).length == sm.getActiveSubscriptionInfoCount();
        });
        step("filter-categories", () -> {
            android.content.IntentFilter f = new android.content.IntentFilter("megler.test.ACTION");
            f.addCategory(android.content.Intent.CATEGORY_DEFAULT);
            java.util.Set<String> both = new java.util.LinkedHashSet<>(java.util.Arrays.asList(android.content.Intent.CATEGORY_DEFAULT, "megler.test.OTHER"));
            String none = f.matchCategories(null), own = f.matchCategories(java.util.Collections.singleton(android.content.Intent.CATEGORY_DEFAULT)), missing = f.matchCategories(both);
            detail = "none=" + none + " own=" + own + " missing=" + missing;
            return none == null && own == null && "megler.test.OTHER".equals(missing);
        });
        // The same rule where the app's manifest is matched: every category of an intent must be its filter's. The phone's home
        // screen (MAIN with CATEGORY_HOME) is no activity of this app's, whose launcher activity (MAIN with CATEGORY_LAUNCHER)
        // was taken for it, and Facebook, looking for a home of another signature, found only itself and ended the app.
        step("intent-categories", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            String own = getPackageName();
            int homes = pm.queryIntentActivities(new android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_HOME), 0).size();
            android.content.pm.ResolveInfo launcher = pm.resolveActivity(new android.content.Intent(android.content.Intent.ACTION_MAIN)
                    .addCategory(android.content.Intent.CATEGORY_LAUNCHER).setPackage(own), 0);
            android.content.pm.ResolveInfo plain = pm.resolveActivity(new android.content.Intent(android.content.Intent.ACTION_MAIN).setPackage(own), 0);
            android.content.pm.ResolveInfo other = pm.resolveActivity(new android.content.Intent(android.content.Intent.ACTION_MAIN)
                    .addCategory("megler.test.NONE").setPackage(own), 0);
            detail = "homes=" + homes + " launcher=" + (launcher == null ? null : launcher.activityInfo.name) + " plain=" + (plain == null ? null : plain.activityInfo.name)
                    + " other=" + (other == null ? null : other.activityInfo.name);
            return homes == 0 && launcher != null && Main.class.getName().equals(launcher.activityInfo.name) && plain != null && other == null;
        });
        // Who sent a broadcast, as Android 14 tells its receivers (getSentFromUid, getSentFromPackage, the receiver's and its
        // PendingResult's): the app itself where the options it sent with share its identity (BroadcastOptions
        // .setShareIdentityEnabled, through the options' bundle, which gives them back), no one where they do not or there are
        // none. Messenger's own receivers refused its own broadcasts, which came from no one ("cannot receive broadcasts from null").
        step("broadcast-identity", () -> {
            Class<?> bo = Class.forName("android.app.BroadcastOptions");
            Object shareOpts = bo.getMethod("makeBasic").invoke(null);
            bo.getMethod("setShareIdentityEnabled", boolean.class).invoke(shareOpts, true);
            android.os.Bundle shared = (android.os.Bundle) bo.getMethod("toBundle").invoke(shareOpts);
            android.os.Bundle plain = (android.os.Bundle) bo.getMethod("toBundle").invoke(bo.getMethod("makeBasic").invoke(null));
            boolean back = (Boolean) bo.getMethod("isShareIdentityEnabled").invoke(bo.getMethod("fromBundle", android.os.Bundle.class).invoke(null, shared));
            final java.lang.reflect.Method uid = android.content.BroadcastReceiver.class.getMethod("getSentFromUid");
            final java.lang.reflect.Method pkg = android.content.BroadcastReceiver.class.getMethod("getSentFromPackage");
            final java.lang.reflect.Method resultUid = android.content.BroadcastReceiver.PendingResult.class.getMethod("getSentFromUid");
            final java.lang.reflect.Method resultPkg = android.content.BroadcastReceiver.PendingResult.class.getMethod("getSentFromPackage");
            final java.util.concurrent.LinkedBlockingQueue<String> heard = new java.util.concurrent.LinkedBlockingQueue<>();
            android.os.HandlerThread ht = new android.os.HandlerThread("identity");
            ht.start();
            final String action = "megler.test.data.IDENTITY";
            android.content.BroadcastReceiver r = new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    try {
                        String mine = uid.invoke(this) + ":" + pkg.invoke(this);
                        PendingResult p = goAsync();
                        heard.add(mine + ":" + resultUid.invoke(p) + ":" + resultPkg.invoke(p));
                        p.finish();
                    } catch (ReflectiveOperationException e) {
                        heard.add(e.toString());
                    }
                }
            };
            registerReceiver(r, new android.content.IntentFilter(action), null, new android.os.Handler(ht.getLooper()));
            java.lang.reflect.Method withOptions = Context.class.getMethod("sendBroadcast", android.content.Intent.class, String.class, android.os.Bundle.class);
            withOptions.invoke(this, new android.content.Intent(action), null, shared);
            String first = heard.poll(10, java.util.concurrent.TimeUnit.SECONDS);
            withOptions.invoke(this, new android.content.Intent(action), null, plain);
            String second = heard.poll(10, java.util.concurrent.TimeUnit.SECONDS);
            sendBroadcast(new android.content.Intent(action));
            String third = heard.poll(10, java.util.concurrent.TimeUnit.SECONDS);
            unregisterReceiver(r);
            ht.quitSafely();
            String me = android.os.Process.myUid() + ":" + getPackageName(), none = "-1:null";
            detail = "back=" + back + " shared=" + first + " not=" + second + " plain=" + third;
            return back && (me + ":" + me).equals(first) && (none + ":" + none).equals(second) && (none + ":" + none).equals(third);
        });
        step("arrays-ranges", () -> {
            Class<?> a = java.util.Arrays.class;
            byte[] one = { 1, 2, 3, 4, 5 }, two = { 9, 2, 3, 4, 8 };
            java.lang.reflect.Method equals = a.getMethod("equals", byte[].class, int.class, int.class, byte[].class, int.class, int.class);
            boolean inner = (Boolean) equals.invoke(null, one, 1, 4, two, 1, 4), whole = (Boolean) equals.invoke(null, one, 0, 5, two, 0, 5);
            int mismatch = (Integer) a.getMethod("mismatch", byte[].class, byte[].class).invoke(null, one, two);
            int shorter = (Integer) a.getMethod("mismatch", byte[].class, byte[].class).invoke(null, new byte[] { 1, 2 }, new byte[] { 1, 2, 3 });
            int ints = (Integer) a.getMethod("compare", int[].class, int[].class).invoke(null, new int[] { 1, 2 }, new int[] { 1, 3 });
            int unsigned = (Integer) a.getMethod("compareUnsigned", byte[].class, byte[].class).invoke(null, new byte[] { (byte) 0xff }, new byte[] { 1 });
            int strings = (Integer) a.getMethod("compare", Comparable[].class, Comparable[].class).invoke(null, new String[] { "a", "b" }, new String[] { "a", "c" });
            boolean nan = (Boolean) a.getMethod("equals", float[].class, int.class, int.class, float[].class, int.class, int.class)
                    .invoke(null, new float[] { Float.NaN }, 0, 1, new float[] { Float.NaN }, 0, 1);
            String refused = "none";
            try {
                equals.invoke(null, one, 3, 2, two, 0, 0);
            } catch (java.lang.reflect.InvocationTargetException e) {
                refused = e.getCause().getClass().getSimpleName();
            }
            detail = "inner=" + inner + " whole=" + whole + " mismatch=" + mismatch + " shorter=" + shorter + " ints=" + ints + " unsigned=" + unsigned
                    + " strings=" + strings + " nan=" + nan + " refused=" + refused;
            return inner && !whole && mismatch == 0 && shorter == 2 && ints < 0 && unsigned > 0 && strings < 0 && nan && "IllegalArgumentException".equals(refused);
        });
        // Java 9 and 10's List.of, Set.of, Map.of, Map.ofEntries, Map.entry and copyOf, which Android has from Android 11 (the
        // libcore overlay's, over java.util.ImmutableCollections): Amazon Shopping's start ended on a NoSuchMethodError of
        // Map.of and Set.of, Shazam's on Map.of() and Map.entry. And Java 10's Collectors.toUnmodifiable* and Java 16's
        // Stream.toList() (Android 13, 14), which make such lists, sets and maps of a stream. Each arity, nulls and duplicates
        // refused, every mutator refused, copies, serialization. By reflection, as above (CollectionFactories.java).
        step("collection-factories", () -> (detail = CollectionFactories.factories()).isEmpty());
        step("unmodifiable-collectors", () -> (detail = CollectionFactories.collectors()).isEmpty());
        // The frame stacks of threads Java starts: two thousand calls deep on a thread of the default size, twenty thousand
        // on one that asked for 16 MB, and a recursion without end the StackOverflowError it is on Android, caught there.
        step("thread-stack", () -> {
            final int[] got = { -1, -1 };
            final boolean[] overflow = { false };
            Thread small = new Thread(() -> { got[0] = depth(2000); try { depth(Integer.MAX_VALUE); } catch (StackOverflowError e) { overflow[0] = true; } });
            Thread big = new Thread(null, () -> got[1] = depth(20000), "deep", 16 << 20);
            small.start();
            big.start();
            small.join(60000);
            big.join(60000);
            detail = "small=" + got[0] + " big=" + got[1] + " overflow=" + overflow[0];
            return got[0] == 2000 && got[1] == 20000 && overflow[0];
        });
        // android.icu.number as an app built for Android 13 calls it (compiled against the app's stubs/): the unit AndroidX's
        // LocalePreferences asks for the weather (Yandex Maps, Google Play services), by the region and by the locale's "mu";
        // the unit for a road's length; Facebook's compact counts; Play Store's fixed fraction digits; a currency, a unit by
        // its full name, the sign always, the value as shown. And ICU 60's own DecimalFormat and CompactDecimalFormat, whose
        // android.icu.number the runtime keeps to core-libart, still format; and ICU 60's Rounder is not the app's to see.
        step("icu-number", () -> {
            android.icu.number.UnlocalizedNumberFormatter weather = android.icu.number.NumberFormatter.with().usage("weather").unit(android.icu.util.MeasureUnit.CELSIUS);
            String us = weather.locale(java.util.Locale.US).format(1).getOutputUnit().getIdentifier();
            String ru = weather.locale(new java.util.Locale("ru", "RU")).format(1).getOutputUnit().getIdentifier();
            String mu = weather.locale(java.util.Locale.forLanguageTag("en-US-u-mu-celsius")).format(1).getOutputUnit().getIdentifier();
            String road = android.icu.number.NumberFormatter.with().usage("road").unit(android.icu.util.MeasureUnit.METER).locale(java.util.Locale.US)
                .format(3218.688).getOutputUnit().getIdentifier();
            String compact = android.icu.number.NumberFormatter.withLocale(java.util.Locale.US).notation(android.icu.number.Notation.compactShort())
                .precision(android.icu.number.Precision.maxFraction(1)).roundingMode(java.math.RoundingMode.DOWN).format(1289000L).toString();
            String compactLong = android.icu.number.NumberFormatter.withLocale(java.util.Locale.US).notation(android.icu.number.Notation.compactLong())
                .format(1289000L).toString();
            String fixed = android.icu.number.NumberFormatter.with().precision(android.icu.number.Precision.fixedFraction(2)).locale(java.util.Locale.US)
                .format(12.3).toString();
            String money = android.icu.number.NumberFormatter.with().unit(android.icu.util.Currency.getInstance("USD")).locale(java.util.Locale.US)
                .format(1234.5).toString();
            String km = android.icu.number.NumberFormatter.with().unit(android.icu.util.MeasureUnit.KILOMETER)
                .unitWidth(android.icu.number.NumberFormatter.UnitWidth.FULL_NAME).locale(java.util.Locale.US).format(5).toString();
            String sign = android.icu.number.NumberFormatter.with().sign(android.icu.number.NumberFormatter.SignDisplay.ALWAYS)
                .grouping(android.icu.number.NumberFormatter.GroupingStrategy.OFF).locale(java.util.Locale.US).format(12345).toString();
            String digits = android.icu.number.NumberFormatter.with().precision(android.icu.number.Precision.maxFraction(3).withMaxDigits(2))
                .locale(java.util.Locale.US).format(123.456).toString();
            java.math.BigDecimal even = android.icu.number.NumberFormatter.with().precision(android.icu.number.Precision.integer())
                .locale(java.util.Locale.US).format(2.5).toBigDecimal();
            String old = android.icu.text.NumberFormat.getInstance(android.icu.util.ULocale.US).format(1234.5);
            String oldCompact = android.icu.text.CompactDecimalFormat.getInstance(android.icu.util.ULocale.US,
                android.icu.text.CompactDecimalFormat.CompactStyle.SHORT).format(1289000L);
            boolean rounder;
            try {
                Class.forName("android.icu.number.Rounder");
                rounder = true;
            } catch (ClassNotFoundException e) {
                rounder = false;
            }
            detail = "us=" + us + " ru=" + ru + " mu=" + mu + " road=" + road + " compact=" + compact + " long=" + compactLong + " fixed=" + fixed
                     + " money=" + money + " km=" + km + " sign=" + sign + " digits=" + digits + " even=" + even + " old=" + old + " old-compact=" + oldCompact
                     + " rounder=" + rounder;
            return "fahrenheit".equals(us) && "celsius".equals(ru) && "celsius".equals(mu) && "mile".equals(road) && "1.2M".equals(compact)
                   && "1.3 million".equals(compactLong) && "12.30".equals(fixed) && "$1,234.50".equals(money) && "5 kilometers".equals(km)
                   && "+12345".equals(sign) && "120".equals(digits) && even.intValue() == 2 && "1,234.5".equals(old) && "1.3M".equals(oldCompact) && !rounder;
        });
        // Where a call's sound goes, as the app's AudioManager has it (WhatsApp's calls): MODE_IN_COMMUNICATION is the
        // earpiece, setSpeakerphoneOn(true) the loudspeaker, and setCommunicationDevice either of the two communication
        // devices there are; MODE_NORMAL ends it. Android 12's methods by reflection: the app is built against Android 10.
        step("audio-route", () -> {
            android.media.AudioManager am = (android.media.AudioManager) getSystemService(AUDIO_SERVICE);
            java.lang.reflect.Method device = android.media.AudioManager.class.getMethod("getCommunicationDevice");
            java.lang.reflect.Method available = android.media.AudioManager.class.getMethod("getAvailableCommunicationDevices");
            java.lang.reflect.Method set = android.media.AudioManager.class.getMethod("setCommunicationDevice", android.media.AudioDeviceInfo.class);
            java.lang.reflect.Method clear = android.media.AudioManager.class.getMethod("clearCommunicationDevice");
            am.setMode(android.media.AudioManager.MODE_IN_COMMUNICATION);
            boolean earpiece = !am.isSpeakerphoneOn()
                               && ((android.media.AudioDeviceInfo) device.invoke(am)).getType() == android.media.AudioDeviceInfo.TYPE_BUILTIN_EARPIECE;
            am.setSpeakerphoneOn(true);
            boolean speaker = am.isSpeakerphoneOn()
                              && ((android.media.AudioDeviceInfo) device.invoke(am)).getType() == android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER;
            java.util.List<?> devices = (java.util.List<?>) available.invoke(am);
            android.media.AudioDeviceInfo ear = null;
            for (Object d : devices)
                if (((android.media.AudioDeviceInfo) d).getType() == android.media.AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) ear = (android.media.AudioDeviceInfo) d;
            boolean chosen = ear != null && (Boolean) set.invoke(am, ear) && !am.isSpeakerphoneOn();
            clear.invoke(am);
            am.setMode(android.media.AudioManager.MODE_NORMAL);
            detail = "earpiece=" + earpiece + " speaker=" + speaker + " devices=" + devices.size() + " chosen=" + chosen + " mode=" + am.getMode();
            return earpiece && speaker && devices.size() == 2 && chosen && am.getMode() == android.media.AudioManager.MODE_NORMAL;
        });
        // TextUtils.expandTemplate as Android has it: a SpannableStringBuilder (Gmail casts it to one), the template's spans
        // kept, ^1 to ^9 replaced and ^^ made ^; a value it does not have asked for is an IllegalArgumentException.
        step("expand-template", () -> {
            android.text.SpannableString t = new android.text.SpannableString("^1 has ^2 ^^ points");
            Object bold = new android.text.style.StyleSpan(android.graphics.Typeface.BOLD);
            t.setSpan(bold, 3, 6, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            CharSequence out = android.text.TextUtils.expandTemplate(t, "Ann", "10");
            boolean builder = out instanceof android.text.SpannableStringBuilder;
            android.text.Spanned sp = (android.text.Spanned) out;
            boolean kept = sp.getSpanStart(bold) == 4 && sp.getSpanEnd(bold) == 7;
            boolean refused;
            try {
                android.text.TextUtils.expandTemplate("^3", "a");
                refused = false;
            } catch (IllegalArgumentException e) {
                refused = true;
            }
            detail = "text=" + out + " builder=" + builder + " span=" + sp.getSpanStart(bold) + ".." + sp.getSpanEnd(bold) + " refused=" + refused;
            return builder && "Ann has 10 ^ points".equals(out.toString()) && kept && refused;
        });
        // A ListFragment as Android's: a list of its own when it makes no view of its own (getListView, an adapter set on it, a
        // row's click to onListItemClick), and the android.R.id.list of a view it makes itself (Gmail's folder list).
        step("list-fragment", () -> {
            final int[] clicked = { -1 };
            final android.widget.BaseAdapter rows = new android.widget.BaseAdapter() {
                @Override public int getCount() { return 2; }
                @Override public Object getItem(int position) { return "row " + position; }
                @Override public long getItemId(int position) { return position; }
                @Override public android.view.View getView(int position, android.view.View convert, android.view.ViewGroup parent) {
                    TextView t = new TextView(parent.getContext());
                    t.setText("row " + position);
                    return t;
                }
            };
            android.app.ListFragment own = new android.app.ListFragment() {
                @Override public void onListItemClick(android.widget.ListView l, android.view.View v, int position, long id) { clicked[0] = position; }
            };
            android.app.ListFragment made = new android.app.ListFragment() {
                @Override public android.view.View onCreateView(android.view.LayoutInflater inflater, android.view.ViewGroup container, Bundle saved) {
                    android.widget.LinearLayout layout = new android.widget.LinearLayout(inflater.getContext());
                    android.widget.ListView list = new android.widget.ListView(inflater.getContext());
                    list.setId(android.R.id.list);
                    layout.addView(list);
                    return layout;
                }
            };
            android.app.FragmentManager fm = getFragmentManager();
            fm.beginTransaction().add(own, "own-list").add(made, "made-list").commit();
            fm.executePendingTransactions();
            android.widget.ListView a = own.getListView(), b = made.getListView();
            own.setListAdapter(rows);
            boolean adapter = a != null && a.getAdapter() == rows && own.getListAdapter() == rows;
            if (a != null) a.performItemClick(null, 1, 1);
            boolean found = b != null && b.getId() == android.R.id.list && b.getParent() instanceof android.widget.LinearLayout;
            fm.beginTransaction().remove(own).remove(made).commit();
            fm.executePendingTransactions();
            detail = "own=" + (a != null) + " adapter=" + adapter + " clicked=" + clicked[0] + " made=" + found;
            return a != null && adapter && clicked[0] == 1 && found;
        });
        // A platform fragment's saved state as Android has it: none (null) for a new fragment, whatever its arguments, and what
        // saveFragmentInstanceState kept of one, through a parcel, for another made with setInitialSavedState (onCreate and
        // onCreateView; onActivityCreated comes after the activity's onCreate, where the steps run). Gmail's conversation view was
        // given its arguments as its saved state and found no view state there.
        step("fragment-saved-state", () -> {
            final Bundle[] seen = new Bundle[3];
            final boolean[] firstCreated = new boolean[1];
            Bundle args = new Bundle();
            args.putString("given", "an argument");
            android.app.Fragment first = new android.app.Fragment() {
                @Override public void onCreate(Bundle saved) { super.onCreate(saved); seen[0] = saved; firstCreated[0] = true; }
                @Override public void onSaveInstanceState(Bundle out) { out.putString("kept", "by the first"); }
            };
            first.setArguments(args);
            android.app.FragmentManager fm = getFragmentManager();
            fm.beginTransaction().add(first, "saved-first").commit();
            fm.executePendingTransactions();
            android.app.Fragment.SavedState kept = fm.saveFragmentInstanceState(first);
            fm.beginTransaction().remove(first).commit();
            fm.executePendingTransactions();
            android.app.Fragment.SavedState back = null;
            if (kept != null) {
                android.os.Parcel p = android.os.Parcel.obtain();
                kept.writeToParcel(p, 0);
                p.setDataPosition(0);
                back = android.app.Fragment.SavedState.CREATOR.createFromParcel(p, getClassLoader());
                p.recycle();
            }
            android.app.Fragment second = new android.app.Fragment() {
                @Override public void onCreate(Bundle saved) { super.onCreate(saved); seen[1] = saved; }
                @Override public android.view.View onCreateView(android.view.LayoutInflater i, android.view.ViewGroup c, Bundle saved) { seen[2] = saved; return null; }
            };
            second.setInitialSavedState(back);
            fm.beginTransaction().add(second, "saved-second").commit();
            fm.executePendingTransactions();
            fm.beginTransaction().remove(second).commit();
            fm.executePendingTransactions();
            boolean fresh = firstCreated[0] && seen[0] == null && first.getArguments() == args;
            boolean restored = back != null && seen[1] != null && "by the first".equals(seen[1].getString("kept")) && seen[2] == seen[1];
            detail = "fresh=" + fresh + " first=" + seen[0] + " kept=" + (kept != null) + " parcelled=" + (back != null) + " second="
                     + (seen[1] == null ? "null" : seen[1].getString("kept")) + " view=" + (seen[2] == seen[1]);
            return fresh && restored;
        });
        // A framework method that is its signature alone, called on purpose: the runtime says its first call, with this caller
        // (mg_dvm_stub_called; the test looks for the line). Fragment.getEnterTransition is one of the generated members.
        step("stub-said", () -> {
            android.app.Fragment f = new android.app.Fragment();
            return f.getEnterTransition() == null;
        });
        // A HandlerThread's looper, as Android gives it: null at once for a thread not started and for one that ended (Facebook's
        // stories loader asked one of those, and waited for good); the thread's own once it runs, with its id.
        step("handler-thread", () -> {
            android.os.HandlerThread never = new android.os.HandlerThread("never started");
            boolean none = never.getLooper() == null && never.getState() == Thread.State.NEW;
            android.os.HandlerThread t = new android.os.HandlerThread("looper");
            t.start();
            android.os.Looper l = t.getLooper();
            boolean runs = l != null && l.getThread() == t && t.getThreadId() > 0;
            t.quit();
            t.join(5000);
            boolean ended = !t.isAlive() && t.getLooper() == null && t.getState() == Thread.State.TERMINATED;
            detail = "none=" + none + " runs=" + runs + " ended=" + ended;
            return none && runs && ended;
        });
        // A dialog the activity makes and keeps itself (Activity.showDialog, as WhatsApp asks whether the phone number is right):
        // made once by onCreateDialog, prepared and shown each time, dismissed, made again after removeDialog.
        step("managed-dialog", () -> {
            dialogsMade = dialogsPrepared = 0;
            showDialog(21);
            android.app.Dialog one = lastDialog;
            boolean shown = one != null && one.isShowing();
            showDialog(21);
            boolean kept = dialogsMade == 1 && dialogsPrepared == 2 && lastDialog == one;
            dismissDialog(21);
            boolean dismissed = !one.isShowing();
            removeDialog(21);
            showDialog(21);
            boolean again = dialogsMade == 2 && lastDialog != one && lastDialog.isShowing();
            dismissDialog(21);
            removeDialog(21);
            boolean none = !showDialog(23, null), unknown = false;
            try {
                dismissDialog(22);
            } catch (IllegalArgumentException e) {
                unknown = true;
            }
            detail = "shown=" + shown + " kept=" + kept + " dismissed=" + dismissed + " again=" + again + " none=" + none + " unknown=" + unknown;
            return shown && kept && dismissed && again && none && unknown;
        });
        // Digits typed into a field whose TextWatcher puts dashes in as one types (WhatsApp's phone number and code fields): each
        // goes where the watcher left the cursor, after its dash, so the digits stay in the order typed; a Backspace at the end
        // takes the last digit. The input connection put the cursor back before the dash, and the next digit went there.
        step("input-format", () -> {
            final android.widget.EditText field = new android.widget.EditText(this);
            field.setInputType(android.text.InputType.TYPE_CLASS_PHONE);
            field.addTextChangedListener(new android.text.TextWatcher() {
                private boolean self;
                @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
                @Override public void afterTextChanged(android.text.Editable s) {
                    if (self) return;
                    StringBuilder digits = new StringBuilder(), shown = new StringBuilder();
                    for (int i = 0; i < s.length(); i++) if (Character.isDigit(s.charAt(i))) digits.append(s.charAt(i));
                    for (int i = 0; i < digits.length(); i++) {
                        if (i == 3 || i == 5) shown.append('-');
                        shown.append(digits.charAt(i));
                    }
                    self = true;
                    s.replace(0, s.length(), shown);
                    android.text.Selection.setSelection(s, s.length());
                    self = false;
                }
            });
            android.view.inputmethod.InputConnection ic = field.onCreateInputConnection(new android.view.inputmethod.EditorInfo());
            for (char c : "1234567".toCharArray()) ic.commitText(String.valueOf(c), 1);
            String typed = field.getText().toString();
            int cursor = field.getSelectionStart();
            ic.deleteSurroundingText(1, 0);
            String back = field.getText().toString();
            detail = "typed=" + typed + " cursor=" + cursor + " after a Backspace=" + back;
            return typed.equals("123-45-67") && cursor == typed.length() && back.equals("123-45-6");
        });
        // The windows an activity opened besides its own, as Android's window manager treats a token's (megler.ViewRoot, asked by
        // reflection): a dialog hidden while its activity is stopped -- told GONE, without the window focus, still attached -- shown
        // again after, and removed once the activity is destroyed. A dialog stayed on the screen over the activities after its own.
        step("dialog-of-stopped", () -> {
            Class<?> roots = Class.forName("megler.ViewRoot");
            java.lang.reflect.Method stopped = roots.getMethod("ownerStopped", Activity.class, boolean.class), close = roots.getMethod("closeAll", Activity.class);
            final int[] seen = { -1 };
            android.view.View probe = new android.view.View(this) {
                @Override protected void onWindowVisibilityChanged(int v) { super.onWindowVisibilityChanged(v); seen[0] = v; }
            };
            android.app.Dialog dialog = new android.app.Dialog(this);
            dialog.setContentView(probe);
            dialog.show();
            android.view.View decor = dialog.getWindow().getDecorView();
            boolean focused = decor.hasWindowFocus();
            stopped.invoke(null, this, true);
            boolean hidden = seen[0] == android.view.View.GONE && !decor.hasWindowFocus() && decor.isAttachedToWindow();
            stopped.invoke(null, this, false);
            boolean shown = seen[0] == android.view.View.VISIBLE && decor.hasWindowFocus();
            int closed = (Integer) close.invoke(null, this);
            boolean gone = closed == 1 && !decor.isAttachedToWindow();
            detail = "focused=" + focused + " hidden=" + hidden + " shown=" + shown + " closed=" + closed + " gone=" + gone;
            return focused && hidden && shown && gone;
        });
        // Android 14's ZipPathValidator, which OsmAnd clears as it starts: there, and a ZIP entry named with ".." still reads.
        step("zip-path-validator", () -> {
            Class<?> v = Class.forName("dalvik.system.ZipPathValidator");
            v.getMethod("clearCallback").invoke(null);
            java.io.File zip = new java.io.File(getCacheDir(), "dots.zip");
            try (java.util.zip.ZipOutputStream o = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(zip))) {
                o.putNextEntry(new java.util.zip.ZipEntry("a/../b.txt"));
                o.write('x');
                o.closeEntry();
            }
            try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(zip)) {
                detail = "entry=" + z.getEntry("a/../b.txt");
                return z.getEntry("a/../b.txt") != null;
            }
        });
        // A list whose row is clickable itself (its own OnClickListener, as Play Store's drawer's rows are): a tap on the row
        // clicks it. The list's interception of the pointer going up unpressed the row first, and the row never clicked.
        step("list-row-click", () -> {
            final boolean[] clicked = { false };
            android.widget.ListView list = new android.widget.ListView(this);
            list.setAdapter(new android.widget.BaseAdapter() {
                @Override public int getCount() { return 3; }
                @Override public Object getItem(int p) { return p; }
                @Override public long getItemId(int p) { return p; }
                @Override public android.view.View getView(int p, android.view.View convert, android.view.ViewGroup parent) {
                    android.widget.TextView t = new android.widget.TextView(Main.this);
                    t.setText("row " + p);
                    t.setMinHeight(60);
                    if (p == 0) t.setOnClickListener(v -> clicked[0] = true);
                    return t;
                }
            });
            list.measure(android.view.View.MeasureSpec.makeMeasureSpec(300, android.view.View.MeasureSpec.EXACTLY),
                         android.view.View.MeasureSpec.makeMeasureSpec(300, android.view.View.MeasureSpec.EXACTLY));
            list.layout(0, 0, 300, 300);
            long now = android.os.SystemClock.uptimeMillis();
            android.view.MotionEvent down = android.view.MotionEvent.obtain(now, now, android.view.MotionEvent.ACTION_DOWN, 20, 20, 0);
            android.view.MotionEvent up = android.view.MotionEvent.obtain(now, now + 50, android.view.MotionEvent.ACTION_UP, 20, 20, 0);
            boolean took = list.dispatchTouchEvent(down);
            list.dispatchTouchEvent(up);
            down.recycle();
            up.recycle();
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() ->
                Log.i("DataTest", "list-row-click rows=" + list.getChildCount() + " clicked=" + clicked[0]), 200);
            detail = "rows=" + list.getChildCount() + " took=" + took;
            return took && list.getChildCount() > 0;
        });
        // A view animation's listener told as Android tells it once the view is in a window: through the view's handler, after
        // the frame that started or ended the animation -- not inside its parent's drawing (Telegram changes its settings list
        // from such a listener, which done inside a RecyclerView's layout ends the app). Said when the animation has ended.
        step("animation-listener", () -> {
            final boolean[] inDraw = new boolean[1];
            final StringBuilder heard = new StringBuilder();
            final android.widget.FrameLayout parent = new android.widget.FrameLayout(this) {
                @Override protected void dispatchDraw(android.graphics.Canvas c) {
                    inDraw[0] = true;
                    try { super.dispatchDraw(c); } finally { inDraw[0] = false; }
                }
            };
            android.view.View child = new android.view.View(this);
            child.setBackgroundColor(0xff336699);
            parent.addView(child, new android.widget.FrameLayout.LayoutParams(20, 20));
            ((android.view.ViewGroup) text.getParent()).addView(parent, new android.view.ViewGroup.LayoutParams(40, 40));
            android.view.animation.AlphaAnimation fade = new android.view.animation.AlphaAnimation(1f, 0.5f);
            fade.setDuration(50);
            fade.setAnimationListener(new android.view.animation.Animation.AnimationListener() {
                @Override public void onAnimationStart(android.view.animation.Animation a) { heard.append(inDraw[0] ? "start-in-draw " : "start "); }
                @Override public void onAnimationRepeat(android.view.animation.Animation a) {}
                @Override public void onAnimationEnd(android.view.animation.Animation a) {
                    heard.append(inDraw[0] ? "end-in-draw" : "end");
                    new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
                        Log.i("DataTest", "animation-listener heard=" + heard);
                        ((android.view.ViewGroup) parent.getParent()).removeView(parent);
                    });
                }
            });
            child.startAnimation(fade);
            return true;
        });
        // A circular reveal played with another animator in an AnimatorSet, as Jamfish plays one as each song begins (the
        // reveal was null, and the set ended the app with a NullPointerException): it is an animator of the radius, and it
        // and the set run to their ends.
        step("circular-reveal", () -> {
            android.view.View v = new android.view.View(this);
            v.layout(0, 0, 100, 50);
            android.animation.Animator reveal = android.view.ViewAnimationUtils.createCircularReveal(v, 50, 25, 0, 112);
            final int[] ended = new int[2];
            final float[] radius = new float[1];
            if (reveal == null) return false;
            ((android.animation.ValueAnimator) reveal).addUpdateListener(a -> radius[0] = (Float) a.getAnimatedValue());
            reveal.addListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(android.animation.Animator a) { ended[0]++; }
            });
            android.animation.AnimatorSet set = new android.animation.AnimatorSet();
            set.play(reveal).with(android.animation.ValueAnimator.ofFloat(0, 1));
            set.setDuration(50);
            set.addListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(android.animation.Animator a) { ended[1]++; }
            });
            set.start();
            set.end();
            detail = "reveal ended " + ended[0] + " at radius " + radius[0] + ", the set ended " + ended[1];
            return ended[0] == 1 && ended[1] == 1 && radius[0] == 112f;
        });
        // An animator's value as Android gives it: 0 before it first runs, and, run once and given new values, the new start
        // value already in its start listeners (it went on as initialised, and they read null).
        step("animator-values", () -> {
            android.animation.ValueAnimator a = android.animation.ValueAnimator.ofFloat(0, 1);
            Object before = a.getAnimatedValue();
            a.setDuration(50);
            a.start();
            a.end();
            a.setValues(android.animation.PropertyValuesHolder.ofFloat("x", 5, 10));
            final Object[] atStart = new Object[1];
            a.addListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationStart(android.animation.Animator x) { atStart[0] = ((android.animation.ValueAnimator) x).getAnimatedValue(); }
            });
            a.start();
            a.end();
            detail = "before " + before + ", at the second start " + atStart[0] + ", at its end " + a.getAnimatedValue();
            return Float.valueOf(0f).equals(before) && atStart[0] instanceof Float && Float.valueOf(10f).equals(a.getAnimatedValue());
        });
        detachedAnimator();
        animatorListener();
        // A view animation's XML is no animator's: AnimatorInflater throws "Unknown animator name" for its <alpha>, alone and
        // in a <set>, as Android does, and AnimationUtils makes the animation of it; an animator's XML is an animator. AndroidX's
        // fragments try an animation resource as an animator first when its type is not named "anim" (Facebook's apps name
        // theirs "anim.2") and take the view animation when that throws (megler/ResInflate.java says what came of the
        // ValueAnimator Megler made of Messenger's <alpha> instead).
        step("animator-not-animation", () -> {
            String alone = null, inSet = null;
            try { android.animation.AnimatorInflater.loadAnimator(this, R.anim.fade_in); } catch (RuntimeException e) { alone = e.getMessage(); }
            try { android.animation.AnimatorInflater.loadAnimator(this, R.anim.fade_set); } catch (RuntimeException e) { inSet = e.getMessage(); }
            android.view.animation.Animation view = android.view.animation.AnimationUtils.loadAnimation(this, R.anim.fade_in);
            android.animation.Animator animator = android.animation.AnimatorInflater.loadAnimator(this, R.animator.fade_in);
            detail = "alone: " + alone + ", in a set: " + inSet + ", the view animation " + view + " of " + (view == null ? 0 : view.getDuration())
                    + " ms, the animator " + animator;
            return "Unknown animator name: alpha".equals(alone) && "Unknown animator name: alpha".equals(inSet)
                    && view instanceof android.view.animation.AlphaAnimation && view.getDuration() == 200
                    && animator instanceof android.animation.ObjectAnimator && animator.getDuration() == 200
                    && "alpha".equals(((android.animation.ObjectAnimator) animator).getPropertyName());
        });
        // A view's observer taken before the view is in a window is dead once it is (merged into the window's): a one-shot
        // listener added to it removes itself through the view's observer then, as AndroidX's OneShotPreDrawListener and
        // YouTube's TraceableOnGlobalLayoutListener do, and is run once (it stayed in the window's, run at every layout), while
        // one left in place hears several layouts. Said after them.
        step("observer-merged", () -> {
            final android.view.View v = new android.view.View(this);
            final android.view.ViewTreeObserver before = v.getViewTreeObserver();
            final int[] runs = new int[2];
            final boolean[] deadWhenRun = new boolean[1];
            final android.view.ViewTreeObserver.OnGlobalLayoutListener layouts = () -> runs[1]++;
            before.addOnGlobalLayoutListener(layouts);
            before.addOnGlobalLayoutListener(new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
                @Override public void onGlobalLayout() {
                    runs[0]++;
                    deadWhenRun[0] = !before.isAlive();
                    if (before.isAlive()) before.removeOnGlobalLayoutListener(this);
                    else v.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                }
            });
            ((android.view.ViewGroup) text.getParent()).addView(v, new android.view.ViewGroup.LayoutParams(10, 10));
            v.postDelayed(() -> {
                // (Three more global layouts told by hand, as a view's code may: dispatchOnGlobalLayout is public.)
                android.view.ViewTreeObserver now = v.getViewTreeObserver();
                for (int k = 0; k < 3; k++) now.dispatchOnGlobalLayout();
                now.removeOnGlobalLayoutListener(layouts);
                Log.i("DataTest", "observer-merged runs=" + runs[0] + " dead=" + deadWhenRun[0] + " layouts=" + (runs[1] >= 3 ? "several" : String.valueOf(runs[1])));
                ((android.view.ViewGroup) v.getParent()).removeView(v);
            }, 500);
            return true;
        });
        // A typeface built from a font file open on a descriptor (Typeface.Builder(FileDescriptor), as Google Play services
        // loads its downloaded fonts): made, and the descriptor still open after.
        step("typeface-descriptor", () -> {
            java.io.File font = new java.io.File("/system/fonts/ComingSoon.ttf");
            try (android.os.ParcelFileDescriptor pfd = android.os.ParcelFileDescriptor.open(font, android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
                android.graphics.Typeface t = new android.graphics.Typeface.Builder(pfd.getFileDescriptor()).build();
                boolean open = pfd.getFileDescriptor().valid();
                detail = "typeface=" + t + " open=" + open;
                return t != null && open;
            }
        });
        // The phone is set up (device_provisioned, user_setup_complete), as it is once its first-use setup is done: Play Store
        // runs no install until it is.
        step("device-provisioned", () -> {
            int global = android.provider.Settings.Global.getInt(getContentResolver(), "device_provisioned", 0);
            int secure = android.provider.Settings.Secure.getInt(getContentResolver(), "user_setup_complete", 0);
            detail = "device_provisioned=" + global + " user_setup_complete=" + secure;
            return global == 1 && secure == 1;
        });
        // The phone's users: one, 0, with serial number 0, its handle found by that number (Play Store counts them before it
        // installs, and ended on the null array it was given; getSerialNumbersOfUsers is a system API, so by reflection).
        step("users", () -> {
            android.os.UserManager um = (android.os.UserManager) getSystemService(USER_SERVICE);
            long[] serials = (long[]) um.getClass().getMethod("getSerialNumbersOfUsers", boolean.class).invoke(um, true);
            android.os.UserHandle first = serials != null && serials.length > 0 ? um.getUserForSerialNumber(serials[0]) : null;
            detail = "serials=" + java.util.Arrays.toString(serials) + " first=" + first;
            return serials != null && serials.length == 1 && first != null && um.getSerialNumberForUser(first) == serials[0];
        });
        step("start-asked", () -> {
            startService(new android.content.Intent(this, Again.class));
            return true;
        });
        // A PendingIntent's creator is the package of the context that made it: GmsCore's code in this process makes its own
        // with its own context, and its task scheduler finds the task's service in that package.
        step("pending-intent-creator", () -> {
            Context other = new android.content.ContextWrapper(this) {
                @Override public String getPackageName() { return "megler.test.other"; }
            };
            android.app.PendingIntent mine = android.app.PendingIntent.getBroadcast(this, 0, new android.content.Intent("megler.test.MINE"), 0);
            android.app.PendingIntent theirs = android.app.PendingIntent.getBroadcast(other, 0, new android.content.Intent("megler.test.THEIRS"), 0);
            detail = "mine " + mine.getCreatorPackage() + ", theirs " + theirs.getCreatorPackage();
            return getPackageName().equals(mine.getCreatorPackage()) && getPackageName().equals(mine.getTargetPackage())
                    && mine.getCreatorUid() == android.os.Process.myUid() && "megler.test.other".equals(theirs.getCreatorPackage());
        });
        // A PendingIntent asked for again by what it starts is the same one, as on Android: with its first extras, or the new
        // ones for FLAG_UPDATE_CURRENT; FLAG_NO_CREATE finds it or gives none, and cancel ends it (GmsCore checks its watchdogs
        // so). The one sent says which extras it had.
        step("pending-intent-flags", () -> {
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    int v = i.getIntExtra("v", 0);
                    Log.i("DataTest", "pending-intent-sent " + (v == 3 ? "ok" : "FAILED") + " v=" + v);
                    c.unregisterReceiver(this);
                }
            }, new android.content.IntentFilter("megler.test.PENDING"));
            android.content.Intent a = new android.content.Intent("megler.test.PENDING").putExtra("v", 1);
            android.app.PendingIntent none = android.app.PendingIntent.getBroadcast(this, 7, a, android.app.PendingIntent.FLAG_NO_CREATE);
            android.app.PendingIntent first = android.app.PendingIntent.getBroadcast(this, 7, a, 0);
            android.app.PendingIntent again = android.app.PendingIntent.getBroadcast(this, 7, new android.content.Intent("megler.test.PENDING").putExtra("v", 2), 0);
            android.app.PendingIntent updated = android.app.PendingIntent.getBroadcast(this, 7, new android.content.Intent("megler.test.PENDING").putExtra("v", 3), android.app.PendingIntent.FLAG_UPDATE_CURRENT);
            android.app.PendingIntent found = android.app.PendingIntent.getBroadcast(this, 7, a, android.app.PendingIntent.FLAG_NO_CREATE);
            android.app.PendingIntent other = android.app.PendingIntent.getBroadcast(this, 8, a, 0);
            boolean sent = true;
            try { first.send(); } catch (android.app.PendingIntent.CanceledException e) { sent = false; }
            first.cancel();
            android.app.PendingIntent gone = android.app.PendingIntent.getBroadcast(this, 7, a, android.app.PendingIntent.FLAG_NO_CREATE);
            boolean refused = false;
            try { first.send(); } catch (android.app.PendingIntent.CanceledException e) { refused = true; }
            android.app.PendingIntent fresh = android.app.PendingIntent.getBroadcast(this, 9, a, 0);
            android.app.PendingIntent instead = android.app.PendingIntent.getBroadcast(this, 9, a, android.app.PendingIntent.FLAG_CANCEL_CURRENT);
            boolean replaced = false;
            try { fresh.send(); } catch (android.app.PendingIntent.CanceledException e) { replaced = true; }
            detail = "none " + (none == null) + ", same " + (again == first) + "/" + (updated == first) + ", found " + (found == first)
                    + ", other " + (other != first) + ", sent " + sent + ", gone " + (gone == null) + ", refused " + refused
                    + ", replaced " + (instead != fresh) + "/" + replaced;
            return none == null && again == first && updated == first && found == first && other != first && sent && gone == null && refused
                    && instead != fresh && replaced;
        });
        // A PendingIntent sent with an OnFinished, a required permission and options, as Google Play services' network location
        // hands its fixes to its fused provider: sent with the fill-in's extras, and the OnFinished told so, on its Handler's
        // thread, with the intent that was sent. Those forms sent nothing.
        step("pending-intent-options", () -> {
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    int v = i.getIntExtra("v", 0);
                    Log.i("DataTest", "pending-intent-options-sent " + (v == 5 ? "ok" : "FAILED") + " v=" + v);
                    c.unregisterReceiver(this);
                }
            }, new android.content.IntentFilter("megler.test.PENDING_OPTIONS"));
            android.app.PendingIntent p = android.app.PendingIntent.getBroadcast(this, 11, new android.content.Intent("megler.test.PENDING_OPTIONS"), 0);
            final java.util.concurrent.CountDownLatch finished = new java.util.concurrent.CountDownLatch(2);
            final int[] heard = new int[2];
            android.os.HandlerThread thread = new android.os.HandlerThread("pending-finished");
            thread.start();
            final android.os.Handler on = new android.os.Handler(thread.getLooper());
            p.send(this, 0, new android.content.Intent().putExtra("v", 5), (pi, intent, code, data, extras) -> {
                heard[0] = intent.getIntExtra("v", 0) + (android.os.Looper.myLooper() == on.getLooper() ? 100 : 0);
                finished.countDown();
            }, on, null, null);
            // (And one only a receiver that holds a permission hears, which none here does: the sender is told all the same.)
            android.app.PendingIntent q = android.app.PendingIntent.getBroadcast(this, 12, new android.content.Intent("megler.test.PENDING_PERMISSION"), 0);
            q.send(this, 0, new android.content.Intent().putExtra("v", 6), (pi, intent, code, data, extras) -> {
                heard[1] = intent.getIntExtra("v", 0);
                finished.countDown();
            }, null, "android.permission.ACCESS_FINE_LOCATION");
            boolean told = finished.await(10, java.util.concurrent.TimeUnit.SECONDS);
            thread.quitSafely();
            detail = "told " + told + ", heard " + heard[0] + "/" + heard[1];
            return told && heard[0] == 105 && heard[1] == 6;
        });
        // Alarms as Android keeps them: one for a PendingIntent, which setting it again moves and cancel takes away, and a
        // repeating one goes off each interval until it is cancelled (GmsCore's watchdog of its module setup is one).
        step("alarm-replace-cancel", () -> {
            // The receiver hears on a thread of its own, as the steps keep the main thread, where alarms go off, busy; it
            // decides once the repeating one has gone off three times, and a while after it cancelled that one.
            final int[] heard = new int[3];
            final android.app.AlarmManager alarms = (android.app.AlarmManager) getSystemService(ALARM_SERVICE);
            final android.os.HandlerThread thread = new android.os.HandlerThread("alarms");
            thread.start();
            final android.os.Handler on = new android.os.Handler(thread.getLooper());
            final android.app.PendingIntent repeated = android.app.PendingIntent.getBroadcast(this, 22, new android.content.Intent("megler.test.ALARM").putExtra("which", 2), 0);
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    int which = i.getIntExtra("which", 0);
                    if (++heard[which] != 3 || which != 2) return;
                    alarms.cancel(repeated);
                    on.postDelayed(() -> {
                        boolean ok = heard[0] == 1 && heard[1] == 0 && heard[2] == 3;
                        Log.i("DataTest", "alarms " + (ok ? "ok" : "FAILED") + " moved=" + heard[0] + " cancelled=" + heard[1] + " repeated=" + heard[2]);
                        c.unregisterReceiver(this);
                        thread.quitSafely();
                    }, 1500);
                }
            }, new android.content.IntentFilter("megler.test.ALARM"), null, on);
            long now = android.os.SystemClock.elapsedRealtime();
            android.app.PendingIntent moved = android.app.PendingIntent.getBroadcast(this, 20, new android.content.Intent("megler.test.ALARM").putExtra("which", 0), 0);
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 100, moved);
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 200, moved);
            android.app.PendingIntent off = android.app.PendingIntent.getBroadcast(this, 21, new android.content.Intent("megler.test.ALARM").putExtra("which", 1), 0);
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 100, off);
            alarms.cancel(off);
            alarms.setRepeating(android.app.AlarmManager.ELAPSED_REALTIME, now + 300, 400, repeated);
            return true;
        });
        // Alarms of a listener, as Android keeps them and Google Play services sets theirs (GmsAlarm, with an Executor and a
        // WorkSource: Android 12's and the system's forms, which did nothing): told on the Handler given or through the Executor
        // given, one for each listener -- set again it moves, cancelled it never goes off -- and a repeating one again each
        // interval until it is cancelled. Everything is heard on a thread of the step's own, which decides once the repeating
        // one has gone off three times, and a while after.
        step("alarm-listeners", () -> {
            final android.app.AlarmManager alarms = (android.app.AlarmManager) getSystemService(ALARM_SERVICE);
            final android.os.HandlerThread thread = new android.os.HandlerThread("alarm-listeners");
            thread.start();
            final android.os.Handler on = new android.os.Handler(thread.getLooper());
            final java.util.concurrent.Executor through = on::post;
            final int[] heard = new int[5];
            final boolean[] onIt = { true };
            final Class<?> listenerClass = android.app.AlarmManager.OnAlarmListener.class;
            long now = android.os.SystemClock.elapsedRealtime();
            android.app.AlarmManager.OnAlarmListener moved = () -> { heard[0]++; onIt[0] &= android.os.Looper.myLooper() == on.getLooper(); };
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 100, "megler-moved", moved, on);
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 300, "megler-moved", moved, on);
            android.app.AlarmManager.OnAlarmListener off = () -> heard[1]++;
            alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, now + 100, "megler-off", off, on);
            alarms.cancel(off);
            android.app.AlarmManager.OnAlarmListener exact = () -> { heard[2]++; onIt[0] &= android.os.Looper.myLooper() == on.getLooper(); };
            android.app.AlarmManager.class.getMethod("setExact", int.class, long.class, String.class, java.util.concurrent.Executor.class, android.os.WorkSource.class, listenerClass)
                .invoke(alarms, android.app.AlarmManager.ELAPSED_REALTIME_WAKEUP, now + 200, "megler-exact", through, new android.os.WorkSource(), exact);
            android.app.AlarmManager.OnAlarmListener window = () -> heard[3]++;
            android.app.AlarmManager.class.getMethod("setWindow", int.class, long.class, long.class, String.class, java.util.concurrent.Executor.class, android.os.WorkSource.class,
                listenerClass).invoke(alarms, android.app.AlarmManager.ELAPSED_REALTIME, now + 200, 1000L, "megler-window", through, new android.os.WorkSource(), window);
            final android.app.AlarmManager.OnAlarmListener[] repeating = new android.app.AlarmManager.OnAlarmListener[1];
            repeating[0] = () -> {
                if (++heard[4] != 3) return;
                alarms.cancel(repeating[0]);
                on.postDelayed(() -> {
                    boolean ok = heard[0] == 1 && heard[1] == 0 && heard[2] == 1 && heard[3] == 1 && heard[4] == 3 && onIt[0];
                    Log.i("DataTest", "alarm-listeners-heard " + (ok ? "ok" : "FAILED") + " moved=" + heard[0] + " cancelled=" + heard[1] + " exact=" + heard[2]
                          + " window=" + heard[3] + " repeated=" + heard[4] + " on-its-thread=" + onIt[0]);
                    thread.quitSafely();
                }, 1500);
            };
            android.app.AlarmManager.class.getMethod("set", int.class, long.class, long.class, long.class, listenerClass, android.os.Handler.class, android.os.WorkSource.class)
                .invoke(alarms, android.app.AlarmManager.ELAPSED_REALTIME, now + 400, 0L, 300L, repeating[0], on, new android.os.WorkSource());
            return true;
        });
        // A wake lock counted as Android counts one: held from acquire to the release that matches it, all at once where it is
        // not counted, and until acquire(timeout)'s time is up (whose release comes on the main thread, which the steps keep
        // busy: a thread of the step's own looks until it is let go). Google Play services checks isHeld as it lets one go,
        // and said "<tag> should be held!" at each.
        step("wake-lock", () -> {
            android.os.PowerManager power = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            final android.os.PowerManager.WakeLock w = power.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "megler:test");
            boolean before = w.isHeld();
            w.acquire();
            w.acquire();
            boolean twice = w.isHeld();
            w.release();
            boolean once = w.isHeld();
            w.release();
            boolean none = w.isHeld();
            w.release();
            boolean extra = w.isHeld();
            final android.os.PowerManager.WakeLock u = power.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "megler:uncounted");
            u.setReferenceCounted(false);
            u.acquire();
            u.acquire();
            u.release();
            boolean uncounted = u.isHeld();
            final android.os.PowerManager.WakeLock timed = power.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "megler:timed");
            timed.acquire(200);
            boolean timedHeld = timed.isHeld();
            detail = "before=" + before + " twice=" + twice + " once=" + once + " none=" + none + " extra=" + extra + " uncounted=" + uncounted + " timed=" + timedHeld;
            final android.os.HandlerThread thread = new android.os.HandlerThread("wake-lock");
            thread.start();
            final android.os.Handler on = new android.os.Handler(thread.getLooper());
            final long since = android.os.SystemClock.elapsedRealtime();
            on.post(new Runnable() {
                @Override public void run() {
                    long waited = android.os.SystemClock.elapsedRealtime() - since;
                    if (timed.isHeld() && waited < 60000) {
                        on.postDelayed(this, 100);
                        return;
                    }
                    Log.i("DataTest", "wake-lock-timed " + (!timed.isHeld() && waited >= 190 ? "ok" : "FAILED") + " held=" + timed.isHeld() + " after " + waited + " ms");
                    thread.quitSafely();
                }
            });
            return !before && twice && once && !none && !extra && !uncounted && timedHeld;
        });
        // The cells around asked for as Android 10 asks (Google Play services' network location, at each of its scans): the
        // callback told through the Executor given, with no cells -- Windows gives an app none -- where it was never told; and
        // Android 12's best-matching network callback (the network location's NetworkConnectionMonitor) told the network is
        // there, where it was never told either.
        step("cell-info-update", () -> {
            android.telephony.TelephonyManager phone = (android.telephony.TelephonyManager) getSystemService(TELEPHONY_SERVICE);
            final java.util.concurrent.CountDownLatch told = new java.util.concurrent.CountDownLatch(2);
            final Object[] cells = new Object[1];
            final java.util.concurrent.ExecutorService executor = java.util.concurrent.Executors.newSingleThreadExecutor();
            phone.requestCellInfoUpdate(executor, new android.telephony.TelephonyManager.CellInfoCallback() {
                @Override public void onCellInfo(java.util.List<android.telephony.CellInfo> list) {
                    cells[0] = list;
                    told.countDown();
                }
            });
            android.net.ConnectivityManager connectivity = (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            final boolean[] available = new boolean[1];
            android.net.ConnectivityManager.NetworkCallback callback = new android.net.ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(android.net.Network n) {
                    available[0] = true;
                    told.countDown();
                }
            };
            android.os.HandlerThread thread = new android.os.HandlerThread("best-matching");
            thread.start();
            android.net.ConnectivityManager.class.getMethod("registerBestMatchingNetworkCallback", android.net.NetworkRequest.class,
                android.net.ConnectivityManager.NetworkCallback.class, android.os.Handler.class)
                .invoke(connectivity, new android.net.NetworkRequest.Builder().addCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET).build(), callback,
                        new android.os.Handler(thread.getLooper()));
            boolean all = told.await(10, java.util.concurrent.TimeUnit.SECONDS);
            connectivity.unregisterNetworkCallback(callback);
            thread.quitSafely();
            executor.shutdown();
            detail = "told=" + all + " cells=" + cells[0] + " available=" + available[0];
            return all && cells[0] instanceof java.util.List && ((java.util.List<?>) cells[0]).isEmpty() && available[0];
        });
        // A Messenger reaches a binder of another's that passes transactions on to a Messenger's, as Google Play services' task
        // services hand theirs to its scheduler, and the message received says the sender's uid (Forward says so).
        step("messenger-forwarded", () -> {
            android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper()) {
                @Override public void handleMessage(android.os.Message m) {
                    boolean ok = m.what == 7 && m.arg1 == 42 && "yes".equals(m.getData().getString("said")) && m.sendingUid == android.os.Process.myUid();
                    Log.i("DataTest", "messenger-forwarded " + (ok ? "ok" : "FAILED") + " what=" + m.what + " arg1=" + m.arg1 + " uid=" + (m.sendingUid == android.os.Process.myUid()));
                }
            };
            android.os.Messenger inner = new android.os.Messenger(main);
            android.os.Message m = android.os.Message.obtain(null, 7, 42, 0);
            m.getData().putString("said", "yes");
            try { new android.os.Messenger(new Forward(inner.getBinder())).send(m); } catch (android.os.RemoteException e) { detail = String.valueOf(e); return false; }
            return true;
        });
        // Insets built one kind of bar at a time, as AndroidX's WindowInsetsCompat builds them on Android 11 and later: what is
        // built has each (Builder.setInsets, Android 11's: through reflection, as this app is built for Android 10).
        step("insets-builder", () -> {
            try {
                Class<?> type = Class.forName("android.view.WindowInsets$Type");
                int status = (Integer) type.getMethod("statusBars").invoke(null), navigation = (Integer) type.getMethod("navigationBars").invoke(null);
                java.lang.reflect.Method set = android.view.WindowInsets.Builder.class.getMethod("setInsets", int.class, android.graphics.Insets.class);
                android.view.WindowInsets.Builder b = new android.view.WindowInsets.Builder();
                set.invoke(b, navigation, android.graphics.Insets.of(0, 0, 0, 168));
                set.invoke(b, status, android.graphics.Insets.of(0, 84, 0, 0));
                android.view.WindowInsets w = b.build();
                detail = "built " + w.getSystemWindowInsetLeft() + "," + w.getSystemWindowInsetTop() + "," + w.getSystemWindowInsetRight() + "," + w.getSystemWindowInsetBottom();
                return w.getSystemWindowInsetTop() == 84 && w.getSystemWindowInsetBottom() == 168 && w.getSystemWindowInsetLeft() == 0;
            } catch (ReflectiveOperationException e) { detail = String.valueOf(e); return false; }
        });
        // A view that hands its insets to its child itself and returns them consumed, as Play Store's content view does: the
        // child has them once, as they were, and not again with nothing left in them (consumed insets end the dispatch).
        step("insets-consumed", () -> {
            android.widget.FrameLayout parent = new android.widget.FrameLayout(this);
            final android.view.View child = new android.view.View(this);
            parent.addView(child);
            final java.util.ArrayList<Integer> got = new java.util.ArrayList<>();
            child.setOnApplyWindowInsetsListener((v, i) -> { got.add(i.getSystemWindowInsetBottom()); return i; });
            parent.setOnApplyWindowInsetsListener((v, i) -> { child.dispatchApplyWindowInsets(i); return i.consumeSystemWindowInsets(); });
            android.view.WindowInsets out = parent.dispatchApplyWindowInsets(new android.view.WindowInsets.Builder().setSystemWindowInsets(android.graphics.Insets.of(0, 0, 0, 168)).build());
            detail = "child had " + got + ", consumed " + out.isConsumed();
            return got.size() == 1 && got.get(0) == 168 && out.isConsumed();
        });
        // Siblings after one that consumes the insets: an app made for Android 11 or later gives each the group's insets, one
        // made for an earlier Android stops at the one that consumed them (this app is made for Android 9; the other is asked
        // for through the framework's record of the app's target, megler.Manifest.targetSdk, and put back).
        step("insets-siblings", () -> {
            java.lang.reflect.Field target;
            try { target = Class.forName("megler.Manifest").getField("targetSdk"); } catch (ReflectiveOperationException e) { detail = String.valueOf(e); return false; }
            int was;
            try { was = target.getInt(null); } catch (IllegalAccessException e) { detail = String.valueOf(e); return false; }
            String[] had = new String[2];
            int[] sdks = { 30, was };
            for (int k = 0; k < 2; k++) {
                android.widget.FrameLayout parent = new android.widget.FrameLayout(this);
                android.view.View first = new android.view.View(this), second = new android.view.View(this);
                parent.addView(first);
                parent.addView(second);
                final java.util.ArrayList<Integer> got = new java.util.ArrayList<>();
                first.setOnApplyWindowInsetsListener((v, i) -> i.consumeSystemWindowInsets());
                second.setOnApplyWindowInsetsListener((v, i) -> { got.add(i.getSystemWindowInsetBottom()); return i; });
                try {
                    target.setInt(null, sdks[k]);
                    parent.dispatchApplyWindowInsets(new android.view.WindowInsets.Builder().setSystemWindowInsets(android.graphics.Insets.of(0, 0, 0, 168)).build());
                } catch (IllegalAccessException e) { detail = String.valueOf(e); return false; }
                finally { try { target.setInt(null, was); } catch (IllegalAccessException e) {} }
                had[k] = String.valueOf(got);
            }
            detail = "target 30: " + had[0] + ", target " + was + ": " + had[1];
            return "[168]".equals(had[0]) && "[]".equals(had[1]);
        });
        // A receiver registered with a Handler hears on that Handler's thread (GmsCore makes blocking calls in such a one).
        step("broadcast-handler", () -> {
            final android.os.HandlerThread thread = new android.os.HandlerThread("broadcast-handler");
            thread.start();
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    boolean held = android.os.Looper.myLooper() == thread.getLooper();
                    Log.i("DataTest", "broadcast-on-handler " + (held ? "ok" : "FAILED") + " thread=" + Thread.currentThread().getName());
                    c.unregisterReceiver(this);
                    thread.quitSafely();
                }
            }, new android.content.IntentFilter("megler.test.ON_HANDLER"), null, new android.os.Handler(thread.getLooper()));
            sendBroadcast(new android.content.Intent("megler.test.ON_HANDLER"));
            return true;
        });
        // Preferences whose keys have spaces (GmsCore keys some by an account, "Account {name=..., type=com.google}"): what
        // one run commits, the next reads from the file whole.
        step("prefs-spaced-key", () -> {
            android.content.SharedPreferences p = getSharedPreferences("spaced", MODE_PRIVATE);
            String key = "Account {name=a b, type=com.example}";
            detail = "found " + p.getAll();
            boolean before = run == 1 || (p.getInt(key, 0) == 7 && "x y".equals(p.getString("with space", null)) && p.getAll().size() == 2);
            return before && p.edit().putInt(key, 7).putString("with space", "x y").commit();
        });
        // An account removed is told as Android 8 and later tell it: ACCOUNT_REMOVED with its name and type, and
        // LOGIN_ACCOUNTS_CHANGED (Play Store learns so, through Google Play services, that its account is gone). Its user data and
        // an auth token set in between are told to no one: LOGIN_ACCOUNTS_CHANGED twice, for the account added and removed.
        step("account-removed", () -> {
            final android.accounts.AccountManager am = android.accounts.AccountManager.get(this);
            final android.accounts.Account a = new android.accounts.Account("removed by run " + run, "megler.test.account");
            final String[] removed = new String[1];
            final int[] changes = new int[1];
            android.content.IntentFilter f = new android.content.IntentFilter(android.accounts.AccountManager.ACTION_ACCOUNT_REMOVED);
            f.addAction(android.accounts.AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION);
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(Context c, android.content.Intent i) {
                    if (android.accounts.AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION.equals(i.getAction())) { changes[0]++; return; }
                    removed[0] = i.getStringExtra(android.accounts.AccountManager.KEY_ACCOUNT_NAME) + "/" + i.getStringExtra(android.accounts.AccountManager.KEY_ACCOUNT_TYPE);
                    boolean held = (a.name + "/" + a.type).equals(removed[0]) && changes[0] == 2;
                    Log.i("DataTest", "account-removed-broadcast " + (held ? "ok" : "FAILED") + " removed=" + removed[0] + " changes=" + changes[0]);
                    c.unregisterReceiver(this);
                }
            }, f);
            if (!am.addAccountExplicitly(a, null, null)) return false;
            am.setUserData(a, "kept", "for a while");
            am.setAuthToken(a, "megler.test.token", "token of run " + run);
            return am.removeAccountExplicitly(a) && am.getAccountsByType(a.type).length == 0;
        });
        // The app's sync adapter, run as Android runs it (Gmail's folders stayed empty, its sync off and not to be turned on): the
        // adapter's type read from its XML, the switches on and kept, and a sync asked for that reaches onPerformSync on a thread
        // of its own with the provider's client, after the account was made syncable by the adapter's autoInitialize (run 1).
        step("sync", () -> {
            final android.accounts.AccountManager am = android.accounts.AccountManager.get(this);
            // (An account type of its own: the account stays for the second run, and "account-removed" counts those of its type.)
            final android.accounts.Account a = new android.accounts.Account("synced", "megler.test.synced");
            if (am.getAccountsByType(a.type).length == 0) am.addAccountExplicitly(a, null, null);
            android.content.SyncAdapterType type = null;
            android.content.SyncAdapterType[] types = android.content.ContentResolver.getSyncAdapterTypes();
            for (android.content.SyncAdapterType t : types != null ? types : new android.content.SyncAdapterType[0])
                if ("megler.test.sync".equals(t.authority)) type = t;
            boolean typed = type != null && a.type.equals(type.accountType) && type.isUserVisible() && !type.supportsUploading() && !type.isAlwaysSyncable();
            int syncable = android.content.ContentResolver.getIsSyncable(a, "megler.test.sync");
            boolean master = android.content.ContentResolver.getMasterSyncAutomatically();
            android.content.ContentResolver.setSyncAutomatically(a, "megler.test.sync", false);
            boolean off = !android.content.ContentResolver.getSyncAutomatically(a, "megler.test.sync");
            android.content.ContentResolver.setSyncAutomatically(a, "megler.test.sync", true);
            boolean on = android.content.ContentResolver.getSyncAutomatically(a, "megler.test.sync");
            final Object listening = android.content.ContentResolver.addStatusChangeListener(android.content.ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE, which -> syncSeen++);
            Bundle extras = new Bundle();
            extras.putBoolean(android.content.ContentResolver.SYNC_EXTRAS_MANUAL, true);
            extras.putBoolean(android.content.ContentResolver.SYNC_EXTRAS_EXPEDITED, true);
            android.content.ContentResolver.requestSync(a, "megler.test.sync", extras);
            detail = "types=" + (types == null ? "null" : types.length) + " typed=" + typed + " syncable=" + syncable + " master=" + master + " off=" + off + " on=" + on
                     + " listening=" + (listening != null);
            return typed && syncable == (run == 1 ? -1 : 1) && master && off && on && listening != null;
        });
        step("package-installer", () -> {
            android.content.pm.PackageInstaller installer = getPackageManager().getPackageInstaller();
            return installer != null && installer == getPackageManager().getPackageInstaller()
                    && installer.getMySessions() != null && installer.getMySessions().isEmpty();
        });
        batchOperations();
        wifi();
        mediaMetadata();
        playbackState();
        remoteUserInfo();
        mediaKeyController();
        containerState();
        contextParams();
        themeAccent();
        phoneApps();
        installs();
        bitmapBytes();
        fileReadOnly();
        componentState();
        adaptiveIcon();
        launcherLoading();
        uriBuilder();
        manyInterfaces();
        proxyManyMethods();
        step("apk-path", () -> {
            // The APK as Android names it, whatever the file is called where the shell keeps it.
            String source = getApplicationInfo().sourceDir, code = getPackageCodePath();
            detail = "sourceDir=" + source + " codePath=" + code + " there=" + new java.io.File(source).isFile();
            return source.equals("/data/app/" + getPackageName() + "-1/base.apk") && source.equals(code) && new java.io.File(source).isFile();
        });
        step("maps-paths", () -> {
            // /proc/self/maps as Android's has it: the APK mapped under its Android path, Android's own libraries where the
            // system keeps them, and none of them in the app's own library folder (Zona takes such a one for an injected library).
            String apk = "/data/app/" + getPackageName() + "-1/base.apk", own = "/data/app/" + getPackageName() + "-1/lib/";
            boolean apkMapped = false, systemLib = false, foreign = false;
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/proc/self/maps"))) {
                for (String line; (line = r.readLine()) != null; ) {
                    if (line.endsWith(" " + apk)) apkMapped = true;
                    if (line.endsWith("/system/lib/libjavacore.so") || line.endsWith("/system/lib64/libjavacore.so")) systemLib = true;
                    if (line.contains(own) && line.endsWith("/libjavacore.so")) foreign = true;
                }
            }
            detail = "apk=" + apkMapped + " javacore in /system=" + systemLib + " in the app's folder=" + foreign;
            return apkMapped && systemLib && !foreign;
        });
        step("apk-optimization", () -> {
            Object info = Class.forName("dalvik.system.ApplicationRuntime")
                    .getMethod("getBaseApkOptimizationInfo").invoke(null);
            return info != null
                    && "run-from-apk".equals(info.getClass().getMethod("getStatus").invoke(info))
                    && "unknown".equals(info.getClass().getMethod("getReason").invoke(info))
                    && Boolean.FALSE.equals(info.getClass().getMethod("isOptimized").invoke(info))
                    && Boolean.FALSE.equals(info.getClass().getMethod("isVerified").invoke(info))
                    && Boolean.FALSE.equals(info.getClass().getMethod("isFullyCompiled").invoke(info));
        });
        insetFraction();
        workSource();
        parcelMarshall();
        packageFlags();
        resourcesLoader();
        locationRequest();
        // A provider's file through a ContentProviderClient, as Google Play services' DynamiteLoader reads its modules'
        // Chimera configuration (acquireUnstableContentProviderClient(uri).openFile(uri, "r")): the provider's own file comes.
        step("provider-client-file", () -> {
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(new java.io.File(getFilesDir(), "provider-config"))) {
                o.write("chimera config".getBytes("UTF-8"));
            }
            android.net.Uri uri = android.net.Uri.parse("content://megler.test.sync/config");
            android.content.ContentProviderClient client = getContentResolver().acquireUnstableContentProviderClient(uri);
            android.os.ParcelFileDescriptor pfd = client == null ? null : client.openFile(uri, "r");
            if (client != null) client.release();
            String read = null;
            if (pfd != null) {
                try (java.io.FileInputStream in = new java.io.FileInputStream(pfd.getFileDescriptor())) {
                    byte[] b = new byte[64];
                    int n = in.read(b);
                    read = n > 0 ? new String(b, 0, n, "UTF-8") : "";
                }
                pfd.close();
            }
            detail = "client=" + client + " read=" + read;
            return "chimera config".equals(read);
        });
        bitmapParcel();
        layoutApiLevel();
        resourcesImpl();
        activityThreadApplication();
        dateFormats();
        roundRectShape();
        visibilityAggregated();
        backgroundOffMainThread();
        singleLineSpans();
        ellipsisNarrower();
        markupRegex();
        audioAttributesEqual();
        notificationChannel();
        sound();
        mediaCodec();
        network();
        renderNodes();
        Log.i("DataTest", "done ok=" + ok + " failed=" + failed);
        doneLogged = true;
        mediaSession();
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
            dialogFragment();
        }, 700);
    }

    /**
     * The Wi-Fi as a game's downloader asks about it before it fetches its data: on and connected by ConnectivityManager and
     * by WifiManager alike, the connection's state and signal, and the address in the connection and in the DHCP answer the same.
     * And an app that may not have the location -- this one names no location permission -- has no scans, as on Android: startScan
     * false and getScanResults an empty list (the test's shell has four networks in range), the connection's name and BSSID
     * hidden.
     */
    private void wifi() {
        step("wifi", () -> {
            android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            android.net.wifi.WifiInfo info = wm.getConnectionInfo();
            android.net.DhcpInfo dhcp = wm.getDhcpInfo();
            android.net.NetworkInfo net = cm.getNetworkInfo(android.net.ConnectivityManager.TYPE_WIFI);
            boolean scanned = wm.startScan();
            java.util.List<android.net.wifi.ScanResult> results = wm.getScanResults();
            detail = "enabled=" + wm.isWifiEnabled() + " state=" + wm.getWifiState() + " info=" + (info == null ? null : info.getSSID() + "/" + info.getSupplicantState()
                    + "/" + info.getRssi() + "/" + info.getIpAddress()) + " dhcp=" + (dhcp == null ? null : dhcp.ipAddress) + " level=" + android.net.wifi.WifiManager.calculateSignalLevel(-50, 5)
                    + " scanned=" + scanned + " results=" + results;
            return !scanned && results != null && results.isEmpty()
                    && wm.isWifiEnabled() && wm.getWifiState() == android.net.wifi.WifiManager.WIFI_STATE_ENABLED && net != null && net.isConnected()
                    && info != null && "<unknown ssid>".equals(info.getSSID()) && info.getSupplicantState() == android.net.wifi.SupplicantState.COMPLETED
                    && "02:00:00:00:00:00".equals(info.getBSSID()) && info.getRssi() == -50 && info.getLinkSpeed() > 0 && dhcp != null && dhcp.ipAddress == info.getIpAddress()
                    && android.net.wifi.WifiManager.calculateSignalLevel(-50, 5) == 4 && android.net.wifi.WifiManager.calculateSignalLevel(-100, 5) == 0
                    && android.net.wifi.WifiManager.calculateSignalLevel(-80, 5) == 1;
        });
    }

    private int dialogsMade, dialogsPrepared;
    private android.app.Dialog lastDialog;

    /** Calls `n` deep ("thread-stack"). */
    static int depth(int n) { return n <= 0 ? 0 : 1 + depth(n - 1); }

    /** The managed dialog of "managed-dialog": made for 21, none for any other number. */
    @Override
    protected android.app.Dialog onCreateDialog(int id) {
        if (id != 21) return null;
        dialogsMade++;
        return new android.app.AlertDialog.Builder(this).setMessage("Is this number right?").setPositiveButton("OK", null).create();
    }

    @Override
    protected void onPrepareDialog(int id, android.app.Dialog dialog) {
        super.onPrepareDialog(id, dialog);
        dialogsPrepared++;
        lastDialog = dialog;
    }

    /** A platform DialogFragment as VK mp3 mod's code check is one: an AlertDialog with a field, made by onCreateDialog. */
    public static class CodeDialog extends android.app.DialogFragment {
        static int created, started, dismissed;
        static final int FIELD = android.view.View.generateViewId();

        @Override
        public android.app.Dialog onCreateDialog(android.os.Bundle saved) {
            created++;
            android.widget.EditText field = new android.widget.EditText(getActivity());
            field.setId(FIELD);
            return new android.app.AlertDialog.Builder(getActivity()).setTitle("Code").setView(field).setPositiveButton("OK", null).create();
        }

        @Override
        public void onStart() {
            super.onStart();
            started++;
        }

        @Override
        public void onDismiss(android.content.DialogInterface d) {
            dismissed++;
            super.onDismiss(d);
        }
    }

    /**
     * Shown with show(FragmentManager, tag), as an app shows one: onCreateDialog made its dialog, which is showing over the
     * activity with the field in it, and the fragment is there by its tag; dismiss() takes the dialog and the fragment away.
     */
    private void dialogFragment() {
        final CodeDialog d = new CodeDialog();
        d.setArguments(new android.os.Bundle());
        d.show(getFragmentManager(), "code");
        new android.os.Handler().postDelayed(() -> {
            final android.app.Dialog dialog = d.getDialog();
            final boolean shown = CodeDialog.created == 1 && CodeDialog.started == 1 && dialog != null && dialog.isShowing() && d.isAdded()
                    && getFragmentManager().findFragmentByTag("code") == d && dialog.findViewById(CodeDialog.FIELD) instanceof android.widget.EditText;
            d.dismiss();
            new android.os.Handler().postDelayed(() -> {
                boolean gone = getFragmentManager().findFragmentByTag("code") == null && d.getDialog() == null && dialog != null && !dialog.isShowing()
                        && CodeDialog.dismissed == 1;
                Log.i("DataTest", "dialog-fragment " + (shown && gone ? "ok" : "FAILED") + " created=" + CodeDialog.created + " started=" + CodeDialog.started
                        + " dismissed=" + CodeDialog.dismissed + " shown=" + shown + " gone=" + gone);
                decorReparent();
            }, 300);
        }, 500);
    }

    /**
     * The decor's content taken out and put in a view of the app's own, which goes in the decor in its place, as an app puts a
     * navigation drawer over the whole window (VK mp3 mod, with a DrawerLayout): the window lays out again and is drawn, the
     * content in the new view, and nothing is cast to the decor's layout parameters it no longer has.
     */
    private void decorReparent() {
        final android.app.Dialog dialog = new android.app.Dialog(this);
        android.widget.TextView text = new android.widget.TextView(this);
        text.setText("moved");
        dialog.setContentView(text);
        dialog.show();
        new android.os.Handler().postDelayed(() -> {
            android.view.ViewGroup decor = (android.view.ViewGroup) dialog.getWindow().getDecorView();
            android.view.View content = decor.getChildAt(0);
            decor.removeView(content);
            android.widget.LinearLayout wrap = new android.widget.LinearLayout(this);
            wrap.addView(content, new android.widget.LinearLayout.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
            decor.addView(wrap);
            decor.requestApplyInsets();
            decor.requestLayout();
            new android.os.Handler().postDelayed(() -> {
                boolean placed = wrap.getParent() == decor && content.getParent() == wrap && text.isShown() && text.getWidth() > 0 && dialog.isShowing();
                Log.i("DataTest", "decor-reparent " + (placed ? "ok" : "FAILED") + " text=" + text.getWidth() + "x" + text.getHeight());
                dialog.dismiss();
                tallDialog();
            }, 400);
        }, 300);
    }

    /**
     * An alert whose message is taller than the screen, as Telegram X's question whether to sync the contacts: the dialog
     * fits on the screen, the message scrolls, and the button is there to be pressed, wholly on the screen.
     */
    private void tallDialog() {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < 120; i++) text.append("A line of a message far longer than the screen is tall, number ").append(i).append('.').append('\n');
        final android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(this).setTitle("Sync the contacts?").setMessage(text)
                .setPositiveButton("OK", null).setNegativeButton("Not now", null).create();
        dialog.show();
        new android.os.Handler().postDelayed(() -> {
            android.widget.Button ok = dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE);
            int[] at = new int[2];
            ok.getLocationOnScreen(at);
            int screen = getResources().getDisplayMetrics().heightPixels;
            android.view.View decor = dialog.getWindow().getDecorView();
            boolean fits = ok.isShown() && ok.getHeight() > 0 && at[1] >= 0 && at[1] + ok.getHeight() <= screen && decor.getHeight() <= screen;
            Log.i("DataTest", "dialog-tall " + (fits ? "ok" : "FAILED") + " button=" + at[1] + "+" + ok.getHeight() + " screen=" + screen + " dialog=" + decor.getHeight());
            dialog.dismiss();
            panelRoot();
        }, 500);
    }

    /**
     * A window of the app's own over the activity (a panel, as King's popups are, with their WebView straight in the root): its
     * root view draws on the window's frame, which is hardware accelerated as a window's canvas is on Android, and a SurfaceView
     * straight in that root draws on one that does not say so, as one does in a list. (A WebView there whose page is drawn on
     * the graphics processor is drawn by its draw functor then; it was drawn in software.) Said as either was seen in any frame.
     */
    private void panelRoot() {
        final boolean[] seen = new boolean[4];
        final android.widget.FrameLayout root = new android.widget.FrameLayout(this) {
            @Override protected void onDraw(android.graphics.Canvas c) { seen[0] = true; seen[1] |= c.isHardwareAccelerated(); }
        };
        root.setWillNotDraw(false);
        android.view.SurfaceView surface = new android.view.SurfaceView(this) {
            @Override public void draw(android.graphics.Canvas c) { seen[2] = true; seen[3] |= c.isHardwareAccelerated(); super.draw(c); }
        };
        root.addView(surface, new android.widget.FrameLayout.LayoutParams(40, 40));
        android.view.WindowManager.LayoutParams lp = new android.view.WindowManager.LayoutParams(200, 200,
                android.view.WindowManager.LayoutParams.TYPE_APPLICATION_PANEL, 0, android.graphics.PixelFormat.TRANSLUCENT);
        lp.token = getWindow().getDecorView().getWindowToken();
        getWindowManager().addView(root, lp);
        new android.os.Handler().postDelayed(() -> {
            boolean ok = seen[0] && seen[1] && seen[2] && !seen[3];
            Log.i("DataTest", "panel-root " + (ok ? "ok" : "FAILED") + " root=" + seen[0] + "/" + seen[1] + " surface=" + seen[2] + "/" + seen[3]);
            getWindowManager().removeView(root);
        }, 500);
    }

    private android.media.session.MediaSession session;

    /**
     * A media session that plays and may be paused, with a title, an artist, an album and a picture of 512 x 256: the test
     * hears its state and what it plays (megler_media_session_host), then sends the phone's next key, which the session does
     * not allow, and its pause key, which must reach onPause on the main thread.
     */
    private void mediaSession() {
        session = new android.media.session.MediaSession(this, "DataTest");
        session.setCallback(new android.media.session.MediaSession.Callback() {
            @Override public void onPause() {
                Log.i("DataTest", "media-session paused" + (android.os.Looper.myLooper() == android.os.Looper.getMainLooper() ? "" : " OFF THE MAIN THREAD"));
            }
            @Override public void onSkipToNext() { Log.i("DataTest", "media-session next FAILED: the state does not allow it"); }
        });
        session.setPlaybackState(new android.media.session.PlaybackState.Builder()
                .setState(android.media.session.PlaybackState.STATE_PLAYING, 1000, 1f)
                .setActions(android.media.session.PlaybackState.ACTION_PAUSE | android.media.session.PlaybackState.ACTION_PLAY).build());
        android.graphics.Bitmap art = android.graphics.Bitmap.createBitmap(512, 256, android.graphics.Bitmap.Config.ARGB_8888);
        art.eraseColor(0xFF336699);
        session.setMetadata(new android.media.MediaMetadata.Builder()
                .putString(android.media.MediaMetadata.METADATA_KEY_TITLE, "Song")
                .putString(android.media.MediaMetadata.METADATA_KEY_ARTIST, "Band")
                .putString(android.media.MediaMetadata.METADATA_KEY_ALBUM, "Record")
                .putBitmap(android.media.MediaMetadata.METADATA_KEY_ART, art).build());
        session.setActive(true);
        Log.i("DataTest", "media-session waiting");
    }

    /**
     * The phone's bars hidden and shown, a step every 300 ms: the status bar by the window's WindowInsetsController (API 30, by
     * reflection: this is built against 29), then both by the decor's system UI flags, each undone after. The test hears
     * 1, 0, 3, 0 (megler_system_bars_host: 1 the status bar, 2 the navigation bar).
     */
    private void bars(android.view.View v) {
        final android.view.Window w = getWindow();
        final Runnable[] steps = {
            () -> statusBar(w, "hide"),
            () -> statusBar(w, "show"),
            () -> w.getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION),
            () -> w.getDecorView().setSystemUiVisibility(0),
        };
        for (int i = 0; i < steps.length; i++) {
            final int k = i;
            v.postDelayed(() -> {
                steps[k].run();
                Log.i("DataTest", "bars-step " + (k + 1));
            }, 300L * (i + 1));
        }
    }

    private static void statusBar(android.view.Window w, String how) {
        try {
            Object controller = android.view.Window.class.getMethod("getInsetsController").invoke(w);
            int status = (Integer) Class.forName("android.view.WindowInsets$Type").getMethod("statusBars").invoke(null);
            Class.forName("android.view.WindowInsetsController").getMethod(how, int.class).invoke(controller, status);
        } catch (Exception e) {
            Log.i("DataTest", "bars " + how + " FAILED: " + e);
        }
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
        /*
         * The app's cache cleared between the runs, as Megler's app page and Android's app info screen ask for it (the test's C
         * side asks the core before the second, megler_ask_clear_cache): what the first run left in its cache folders -- its
         * private cache and code_cache, its own Android/data/<package>/cache -- is gone in the second, the folders there; what it
         * left in its files folders stays.
         */
        step("cache-cleared", () -> {
            File[] cached = { new File(getCacheDir(), "run-1-cache.txt"), new File(getCodeCacheDir(), "run-1-code.txt"),
                              new File(getExternalCacheDir(), "run-1-external-cache.txt") };
            File[] kept = { new File(getFilesDir(), "run-1-files.txt"), new File(getExternalFilesDir(null), "run-1-external-files.txt") };
            boolean ok = true;
            StringBuilder said = new StringBuilder();
            for (File f : cached) {
                if (run == 1) try (java.io.FileWriter w = new java.io.FileWriter(f)) { w.write("cached in run 1\n"); }
                said.append(f.getName()).append('=').append(f.exists()).append(' ');
                ok &= f.exists() == (run == 1) && f.getParentFile().isDirectory();
            }
            for (File f : kept) {
                if (run == 1) try (java.io.FileWriter w = new java.io.FileWriter(f)) { w.write("kept from run 1\n"); }
                said.append(f.getName()).append('=').append(f.exists()).append(' ');
                ok &= f.exists();
            }
            detail = said.toString();
            return ok;
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
            String read = android.Manifest.permission.READ_EXTERNAL_STORAGE, write = android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                   camera = android.Manifest.permission.CAMERA, vibrate = android.Manifest.permission.VIBRATE, contacts = android.Manifest.permission.READ_CONTACTS,
                   calendar = android.Manifest.permission.READ_CALENDAR, sms = android.Manifest.permission.RECEIVE_SMS;
            int granted = android.content.pm.PackageManager.PERMISSION_GRANTED, denied = android.content.pm.PackageManager.PERMISSION_DENIED;
            android.content.pm.PackageManager pm = getPackageManager();
            android.content.pm.PackageInfo info = pm.getPackageInfo(getPackageName(), android.content.pm.PackageManager.GET_PERMISSIONS);
            java.util.List<String> asked = Arrays.asList(info.requestedPermissions);
            int readFlags = info.requestedPermissionsFlags[asked.indexOf(read)], vibrateFlags = info.requestedPermissionsFlags[asked.indexOf(vibrate)];
            android.content.pm.PermissionInfo cameraInfo = pm.getPermissionInfo(camera, 0), vibrateInfo = pm.getPermissionInfo(vibrate, 0);
            /* What the user said in the first run is the shell's to keep: the second finds the storage and the messages granted. */
            boolean first = run == 1;
            detail = "read=" + checkSelfPermission(read) + " sms=" + checkSelfPermission(sms) + " camera=" + checkSelfPermission(camera) + " write=" + checkSelfPermission(write)
                    + " contacts=" + checkSelfPermission(contacts) + " calendar=" + checkSelfPermission(calendar) + " vibrate=" + checkSelfPermission(vibrate)
                    + " rationale=" + shouldShowRequestPermissionRationale(camera) + " asked=" + asked + " flags=" + readFlags + "/" + vibrateFlags
                    + " info=" + cameraInfo.protectionLevel + "/" + cameraInfo.group + "/" + vibrateInfo.protectionLevel;
            /*
             * Before the request: a normal permission granted with the install; one the manifest does not ask for, and one asked for
             * only up to Android 9, denied and not even among those it asks for; the contacts denied, with no rationale -- a group
             * the shell has nothing behind and grants with no one asked, which the user refused in the app's App Permissions; the
             * dangerous ones of the run before as the user left them, of the first run not yet; no rationale before a no.
             */
            boolean held = checkSelfPermission(vibrate) == granted && checkSelfPermission(calendar) == denied && checkSelfPermission(write) == denied
                    && checkSelfPermission(contacts) == denied && !shouldShowRequestPermissionRationale(contacts)
                    && pm.checkPermission(contacts, getPackageName()) == denied && checkSelfPermission(read) == (first ? denied : granted)
                    && checkSelfPermission(sms) == (first ? denied : granted) && checkSelfPermission(camera) == denied
                    && !shouldShowRequestPermissionRationale(camera) && !shouldShowRequestPermissionRationale(vibrate)
                    && checkCallingOrSelfPermission(vibrate) == granted && checkPermission(read, android.os.Process.myPid(), android.os.Process.myUid()) == (first ? denied : granted)
                    && pm.checkPermission(read, getPackageName()) == (first ? denied : granted) && pm.checkPermission(calendar, getPackageName()) == denied
                    && asked.contains(read) && !asked.contains(write) && (vibrateFlags & android.content.pm.PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0
                    && ((readFlags & android.content.pm.PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0) == !first
                    && cameraInfo.protectionLevel == android.content.pm.PermissionInfo.PROTECTION_DANGEROUS && "android.permission-group.CAMERA".equals(cameraInfo.group)
                    && vibrateInfo.protectionLevel == android.content.pm.PermissionInfo.PROTECTION_NORMAL;
            /* The request (answered later, onRequestPermissionsResult); and a second while it is answered, told at once it was cancelled. */
            requestPermissions(PERMISSIONS, 77);
            cancelled = false;
            requestPermissions(new String[] { camera }, 78);
            return held && cancelled;
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
     * with each other; what the numbers are is the test's to say, which sets them in the core (mg_battery_set) before the run.
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
     * The phone's text messages and calls, which the test's C side gives the app as the shell would (megler_sms_received,
     * megler_call_logged) once this says it is ready: SMS_RECEIVED read with getMessagesFromIntent (sender, time, the text of
     * all its parts) and the message in content://sms/inbox; the PendingIntent of an app-specific SMS token, sent by the
     * message that holds the token; PHONE_STATE RINGING with the caller's number and then IDLE, TelephonyManager's listener the
     * same, and the call in content://call_log/calls. Each is a line; the C side checks them.
     */
    private void phoneEvents() {
        registerReceiver(new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context context, android.content.Intent intent) {
                android.telephony.SmsMessage[] parts = android.provider.Telephony.Sms.Intents.getMessagesFromIntent(intent);
                StringBuilder body = new StringBuilder();
                for (android.telephony.SmsMessage m : parts) body.append(m.getMessageBody());
                String from = parts[0].getOriginatingAddress();
                Log.i("DataTest", "sms-heard parts=" + parts.length + " from=" + from + " time=" + parts[0].getTimestampMillis() + " body=" + body);
                try (Cursor c = getContentResolver().query(android.provider.Telephony.Sms.Inbox.CONTENT_URI, new String[] { "address", "body", "type" },
                                                           "address=?", new String[] { from }, null)) {
                    String got = c != null && c.moveToFirst() ? c.getString(1) + " type=" + c.getInt(2) : "none";
                    Log.i("DataTest", "sms-inbox rows=" + (c == null ? -1 : c.getCount()) + " body=" + got);
                }
            }
        }, new android.content.IntentFilter("android.provider.Telephony.SMS_RECEIVED"));
        registerReceiver(new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context context, android.content.Intent intent) {
                android.telephony.SmsMessage[] parts = android.provider.Telephony.Sms.Intents.getMessagesFromIntent(intent);
                Log.i("DataTest", "sms-token-heard parts=" + (parts == null ? 0 : parts.length) + " from=" + (parts == null ? "" : parts[0].getOriginatingAddress()));
            }
        }, new android.content.IntentFilter("megler.test.SMS_TOKEN"));
        android.app.PendingIntent pi = android.app.PendingIntent.getBroadcast(this, 7, new android.content.Intent("megler.test.SMS_TOKEN").setPackage(getPackageName()), 0);
        String token = android.telephony.SmsManager.getDefault().createAppSpecificSmsToken(pi);
        registerReceiver(new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context context, android.content.Intent intent) {
                String state = intent.getStringExtra(android.telephony.TelephonyManager.EXTRA_STATE);
                Log.i("DataTest", "call-state " + state + " number=" + intent.getStringExtra(android.telephony.TelephonyManager.EXTRA_INCOMING_NUMBER));
                if (!android.telephony.TelephonyManager.EXTRA_STATE_IDLE.equals(state)) return;
                try (Cursor c = getContentResolver().query(android.provider.CallLog.Calls.CONTENT_URI, new String[] { "number", "type", "duration" },
                                                           "type=?", new String[] { "3" }, "date DESC")) {
                    String got = c != null && c.moveToFirst() ? c.getString(0) + " type=" + c.getInt(1) : "none";
                    Log.i("DataTest", "call-log rows=" + (c == null ? -1 : c.getCount()) + " number=" + got);
                }
            }
        }, new android.content.IntentFilter(android.telephony.TelephonyManager.ACTION_PHONE_STATE_CHANGED));
        android.telephony.TelephonyManager tm = (android.telephony.TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
        tm.listen(new android.telephony.PhoneStateListener() {
            @Override
            public void onCallStateChanged(int state, String number) { Log.i("DataTest", "call-listener " + state + " " + number); }
        }, android.telephony.PhoneStateListener.LISTEN_CALL_STATE);
        Log.i("DataTest", "phone-ready token=" + token);
    }

    /**
     * The network, as the sticky CONNECTIVITY_ACTION says it, which is how an app learns the network it starts on: registerReceiver
     * gives the intent back, and a receiver registered for it is called with it, once, later, on the main thread (Asphalt 9 tells
     * its native code the connection's type from there and from nowhere else). It says what ConnectivityManager says.
     */
    /*
     * The phone's location (the tests' stand-in puts it in Moscow, the location on): the providers there are and on, the newest
     * fix named for the provider asked, a Location through a Parcel and back, the distance from Moscow to Saint Petersburg on
     * the ellipsoid (636 215 m, Vincenty's), and an update to a listener on the main thread, after which it is removed.
     */
    private void location() {
        step("location", () -> {
            final android.location.LocationManager lm = (android.location.LocationManager) getSystemService(Context.LOCATION_SERVICE);
            boolean gps = lm.isProviderEnabled(android.location.LocationManager.GPS_PROVIDER), on = lm.isLocationEnabled();
            java.util.List<String> all = lm.getAllProviders();
            android.location.Location last = lm.getLastKnownLocation(android.location.LocationManager.NETWORK_PROVIDER);
            android.os.Parcel parcel = android.os.Parcel.obtain();
            boolean back = false;
            if (last != null) {
                last.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                back = last.equals(android.location.Location.CREATOR.createFromParcel(parcel));
            }
            parcel.recycle();
            float[] d = new float[3];
            android.location.Location.distanceBetween(55.751244, 37.618423, 59.938630, 30.314130, d);
            lm.requestLocationUpdates(android.location.LocationManager.GPS_PROVIDER, 0, 0, new android.location.LocationListener() {
                private int calls;

                @Override
                public void onLocationChanged(android.location.Location l) {
                    if (++calls != 1) return;
                    boolean good = android.location.LocationManager.GPS_PROVIDER.equals(l.getProvider()) && Math.abs(l.getLatitude() - 55.7512) < 0.01
                            && l.hasAccuracy() && l.getAccuracy() == 12f && l.hasAltitude() && l.getTime() > 0 && l.getElapsedRealtimeNanos() > 0
                            && android.os.Looper.myLooper() == android.os.Looper.getMainLooper();
                    Log.i("DataTest", "location-update " + (good ? "ok" : "FAILED " + l));
                    lm.removeUpdates(this);
                }

                // (Abstract in the android.jar the app is built against; Android 11 gave them defaults.)
                @Override
                public void onProviderEnabled(String provider) {}

                @Override
                public void onProviderDisabled(String provider) {}

                @Override
                public void onStatusChanged(String provider, int status, android.os.Bundle extras) {}
            });
            detail = "gps=" + gps + " on=" + on + " providers=" + all + " last=" + last + " parcel=" + back + " distance=" + d[0] + " bearing=" + d[1];
            return gps && on && all.contains(android.location.LocationManager.NETWORK_PROVIDER) && all.contains("fused")
                    && last != null && android.location.LocationManager.NETWORK_PROVIDER.equals(last.getProvider()) && back
                    && Math.abs(d[0] - 636215f) < 1f && d[1] < -35 && d[1] > -45;
        });
    }

    /*
     * The phone's sensors (the tests' stand-in has one of each kind Windows has, each reading a known value in Windows' units
     * and signs): the list as Android describes it, the proximity the default that wakes the device, and each sensor's first
     * event in Android's units and signs -- the accelerometer's z +7.35 m/s^2 where Windows reads -0.75 g -- on the main
     * thread, with onAccuracyChanged before it, its time on elapsedRealtimeNanos's clock (read after the listener was
     * registered, and before it is handed over: the main thread runs the other steps first, which under qemu takes long
     * enough for a hundred of a sensor's events to wait, the most a listener's thread is sent); the accelerometer's next ones at
     * about the rate asked for (SENSOR_DELAY_GAME, 20 ms, while the stand-in reads every 5 ms); then a flush of all eleven,
     * after which the listener is removed. What came is one line, "sensors-events".
     */
    private void sensors() {
        step("sensors", () -> {
            final android.hardware.SensorManager sm = (android.hardware.SensorManager) getSystemService(Context.SENSOR_SERVICE);
            java.util.List<android.hardware.Sensor> all = sm.getSensorList(android.hardware.Sensor.TYPE_ALL);
            android.hardware.Sensor accel = sm.getDefaultSensor(android.hardware.Sensor.TYPE_ACCELEROMETER);
            android.hardware.Sensor prox = sm.getDefaultSensor(android.hardware.Sensor.TYPE_PROXIMITY);
            StringBuilder types = new StringBuilder();
            for (android.hardware.Sensor s : all) types.append(s.getType()).append(',');
            boolean listed = all.size() == 11 && "1,2,3,4,5,6,8,9,10,11,15,".equals(types.toString()) && accel != null
                    && "Test Accelerometer".equals(accel.getName()) && "Megler Tests".equals(accel.getVendor()) && accel.getMinDelay() == 10000
                    && Math.abs(accel.getMaximumRange() - 39.2266f) < 0.001f && accel.getReportingMode() == android.hardware.Sensor.REPORTING_MODE_CONTINUOUS
                    && !accel.isWakeUpSensor() && "android.sensor.accelerometer".equals(accel.getStringType()) && prox != null && prox.isWakeUpSensor()
                    && prox.getMinDelay() == 0 && prox.getMaximumRange() == 5f
                    && sm.getDefaultSensor(android.hardware.Sensor.TYPE_PROXIMITY, false) == null
                    && sm.getSensorList(android.hardware.Sensor.TYPE_GYROSCOPE).size() == 1
                    && sm.getDefaultSensor(android.hardware.Sensor.TYPE_HEART_RATE) == null
                    && "Rotation Vector Sensor".equals(sm.getDefaultSensor(android.hardware.Sensor.TYPE_ROTATION_VECTOR).getName());
            final SensorCheck check = new SensorCheck(sm, android.os.SystemClock.elapsedRealtimeNanos());
            boolean registered = true;
            for (android.hardware.Sensor s : all)
                registered &= sm.registerListener(check, s, s == accel ? android.hardware.SensorManager.SENSOR_DELAY_GAME : android.hardware.SensorManager.SENSOR_DELAY_NORMAL);
            // (The same sensor again for the same listener is refused, as on Android.)
            boolean again = sm.registerListener(check, accel, android.hardware.SensorManager.SENSOR_DELAY_FASTEST);
            detail = "types=" + types + " accel=" + accel + " prox=" + prox + " registered=" + registered + " again=" + again;
            return listed && registered && !again;
        });
    }

    /** What sensors() hears: each sensor's first event against the stand-in's reading, the accelerometer's for their rate. */
    private static final class SensorCheck implements android.hardware.SensorEventListener2 {
        private final android.hardware.SensorManager sm;
        private final long registered;
        private final java.util.TreeMap<Integer, String> first = new java.util.TreeMap<>();
        private int accelEvents, accuracyCalls, flushed;
        private long lastAccel, minGap = Long.MAX_VALUE;
        private boolean ordered = true, flushing, done;

        SensorCheck(android.hardware.SensorManager sm, long registered) {
            this.sm = sm;
            this.registered = registered;
        }

        /** Android's values for each sensor of the stand-in's (core/tests/mg_sensor_standin.c), and their accuracy. */
        private static float[] expected(int type) {
            switch (type) {
            case android.hardware.Sensor.TYPE_ACCELEROMETER: return new float[] { -2.4516625f, 4.903325f, 7.3549875f, 3 };
            case android.hardware.Sensor.TYPE_MAGNETIC_FIELD: return new float[] { 20f, -30f, -40f, 2 };
            case android.hardware.Sensor.TYPE_ORIENTATION: return new float[] { 70.3141f, -31.5667f, 17.2294f, 3 };
            case android.hardware.Sensor.TYPE_GYROSCOPE: return new float[] { 1.5707963f, -0.7853982f, 3.1415927f, 3 };
            case android.hardware.Sensor.TYPE_LIGHT: return new float[] { 321.5f, 3 };
            case android.hardware.Sensor.TYPE_PRESSURE: return new float[] { 1013.25f, 3 };
            case android.hardware.Sensor.TYPE_PROXIMITY: return new float[] { 3f, 3 };
            case android.hardware.Sensor.TYPE_GRAVITY: return new float[] { 0f, 5.88399f, 7.84532f, 3 };
            case android.hardware.Sensor.TYPE_LINEAR_ACCELERATION: return new float[] { -0.980665f, -1.96133f, 2.941995f, 3 };
            case android.hardware.Sensor.TYPE_ROTATION_VECTOR: return new float[] { -0.1f, -0.2f, -0.3f, 0.92736185f, -1f, 3 };
            case android.hardware.Sensor.TYPE_GAME_ROTATION_VECTOR: return new float[] { 0f, 0f, 0.6f, 0.8f, 3 };
            default: return null;
            }
        }

        @Override
        public void onAccuracyChanged(android.hardware.Sensor s, int accuracy) {
            accuracyCalls++;
        }

        @Override
        public void onSensorChanged(android.hardware.SensorEvent e) {
            if (done) return;
            long now = android.os.SystemClock.elapsedRealtimeNanos();
            int type = e.sensor.getType();
            if (type == android.hardware.Sensor.TYPE_ACCELEROMETER) {
                if (accelEvents > 0) {
                    if (e.timestamp <= lastAccel) ordered = false;
                    minGap = Math.min(minGap, e.timestamp - lastAccel);
                }
                lastAccel = e.timestamp;
                accelEvents++;
            }
            if (!first.containsKey(type)) {
                float[] want = expected(type);
                boolean good = want != null && e.values.length == want.length - 1 && e.accuracy == (int) want[want.length - 1]
                        && e.timestamp <= now && e.timestamp >= registered - 50_000_000L && android.os.Looper.myLooper() == android.os.Looper.getMainLooper();
                for (int i = 0; good && i < e.values.length; i++) good = Math.abs(e.values[i] - want[i]) < 0.001f;
                first.put(type, good ? "ok" : Arrays.toString(e.values) + "/" + e.accuracy + "@" + (e.timestamp - registered));
            }
            if (!flushing && first.size() == 11 && accelEvents >= 6) {
                flushing = true;
                if (!sm.flush(this)) finish("flush=false");
            }
        }

        @Override
        public void onFlushCompleted(android.hardware.Sensor s) {
            if (++flushed == 11) finish("");
        }

        private void finish(String why) {
            done = true;
            sm.unregisterListener(this);
            boolean all = true;
            for (String v : first.values()) all &= "ok".equals(v);
            boolean good = why.isEmpty() && all && ordered && minGap >= 16_000_000L && accuracyCalls == 11 && flushed == 11;
            Log.i("DataTest", "sensors-events " + (good ? "ok" : "FAILED " + why) + " first=" + first + " accel=" + accelEvents + " gap=" + minGap / 1000
                    + "us ordered=" + ordered + " accuracy-calls=" + accuracyCalls + " flushed=" + flushed);
        }
    }

    /*
     * The name servers of the network in use, as a resolver of an app's own asks for them (c-ares, inside 2GIS's map library):
     * ConnectivityManager's LinkProperties of the active network. The host's (/etc/resolv.conf under qemu, the phone's
     * interfaces on Windows), each a literal address.
     */
    private void dnsServers() {
        step("dns-servers", () -> {
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            android.net.LinkProperties lp = cm.getLinkProperties(cm.getActiveNetwork());
            java.util.List<java.net.InetAddress> dns = lp == null ? null : lp.getDnsServers();
            Log.i("DataTest", "dns-servers " + dns);
            detail = String.valueOf(dns);
            return dns != null && !dns.isEmpty() && dns.get(0) != null && dns.get(0).getHostAddress() != null;
        });
    }

    /**
     * The system's libraries as files in /system/lib, where Facebook's SoLoader looks for an app library's system dependencies
     * before loading it (Instagram's libz.so), and System.loadLibrary of one of them, which finds that file and loads the
     * library by its name. A name that is no library of the system's is no file there.
     */
    private void systemLibs() {
        step("system-libs", () -> {
            boolean files = new java.io.File("/system/lib/libz.so").isFile() && new java.io.File("/system/lib/libandroid.so").isFile()
                    && new java.io.File("/system/lib/liblog.so").isFile() && !new java.io.File("/system/lib/libno-such-thing.so").exists();
            String loaded;
            try {
                System.loadLibrary("z");
                loaded = "ok";
            } catch (UnsatisfiedLinkError e) {
                loaded = e.getMessage();
            }
            detail = "files=" + files + " loadLibrary(z)=" + loaded;
            return files && "ok".equals(loaded);
        });
    }

    /**
     * Android's legacy Apache HTTP client, which this app's manifest asks for (<uses-library android:name="org.apache.http.legacy">),
     * as Instagram's does: org.apache.http.Header there for its native code to find, a BasicHeader made and read, and the
     * classes the app's class loader's, as a shared library's are on Android. By reflection: the SDK's android.jar has none of it.
     */
    private void apacheHttpLegacy() {
        step("apache-http-legacy", () -> {
            Class<?> header = Class.forName("org.apache.http.Header");
            Class<?> basic = Class.forName("org.apache.http.message.BasicHeader");
            Object h = basic.getConstructor(String.class, String.class).newInstance("X-Megler", "legacy");
            String name = (String) header.getMethod("getName").invoke(h), value = (String) header.getMethod("getValue").invoke(h);
            boolean appLoader = header.getClassLoader() == Main.class.getClassLoader();
            detail = name + ": " + value + ", the app's class loader " + appLoader;
            return "X-Megler".equals(name) && "legacy".equals(value) && appLoader;
        });
        /* Its client made as Google's GSF makes it (GoogleHttpClient): AndroidHttpClient.newInstance with the app's context,
           whose https is the platform's SSLCertificateSocketFactory.getHttpSocketFactory (a NoSuchMethodError before,
           package 576), on 443, a socket factory of Apache's that says its sockets are secure. */
        step("apache-http-client", () -> {
            Class<?> clientClass = Class.forName("android.net.http.AndroidHttpClient");
            Object client = clientClass.getMethod("newInstance", String.class, android.content.Context.class).invoke(null, "MeglerTest", this);
            Object manager = clientClass.getMethod("getConnectionManager").invoke(client);
            Object registry = Class.forName("org.apache.http.conn.ClientConnectionManager").getMethod("getSchemeRegistry").invoke(manager);
            Object https = registry.getClass().getMethod("getScheme", String.class).invoke(registry, "https");
            Object factory = https.getClass().getMethod("getSocketFactory").invoke(https);
            int port = (Integer) https.getClass().getMethod("getDefaultPort").invoke(https);
            boolean layered = Class.forName("org.apache.http.conn.scheme.LayeredSocketFactory").isInstance(factory);
            clientClass.getMethod("close").invoke(client);
            detail = "https on " + port + " through " + factory.getClass().getName() + ", layered " + layered;
            return port == 443 && "org.apache.http.conn.ssl.SSLSocketFactory".equals(factory.getClass().getName()) && layered;
        });
    }

    /**
     * The platform's preloaded drawables, as Instagram fills them: the static field sPreloadedDrawables found by reflection
     * (ResourcesImpl's, else Resources's), each array element swapped for a LongSparseArray of the app's that answers a key
     * made as Android makes it -- the value's asset cookie in the high word, its data in the low -- and a drawable resource
     * of that key then comes from it, not from its file; with the element put back, from its file again.
     */
    private void preloadedDrawables() {
        step("preloaded-drawables", () -> {
            Class<?> holder;
            try {
                holder = Class.forName("android.content.res.ResourcesImpl");
            } catch (ClassNotFoundException e) {
                holder = android.content.res.Resources.class;
            }
            java.lang.reflect.Field f = holder.getDeclaredField("sPreloadedDrawables");
            f.setAccessible(true);
            Object field = f.get(null);
            if (!(field instanceof android.util.LongSparseArray[])) {
                detail = "sPreloadedDrawables is " + field;
                return false;
            }
            android.util.LongSparseArray[] arrays = (android.util.LongSparseArray[]) field;
            final int id = android.R.drawable.ic_menu_compass;
            android.util.TypedValue tv = new android.util.TypedValue();
            getResources().getValue(id, tv, false);
            final long key = (((long) tv.assetCookie) << 32) | tv.data;
            final android.graphics.drawable.Drawable.ConstantState mine = new android.graphics.drawable.Drawable.ConstantState() {
                @Override public android.graphics.drawable.Drawable newDrawable() { return new android.graphics.drawable.ColorDrawable(0xFF123456); }
                @Override public int getChangingConfigurations() { return 0; }
            };
            android.util.LongSparseArray before = arrays[0];
            final android.util.LongSparseArray inner = before;
            arrays[0] = new android.util.LongSparseArray<Object>() {
                @Override public Object get(long k) { return k == key ? mine : inner.get(k); }
            };
            android.graphics.drawable.Drawable answered;
            try {
                answered = getResources().getDrawable(id, null);
            } finally {
                arrays[0] = before;
            }
            android.graphics.drawable.Drawable fromFile = getResources().getDrawable(id, null);
            boolean ok = answered instanceof android.graphics.drawable.ColorDrawable && ((android.graphics.drawable.ColorDrawable) answered).getColor() == 0xFF123456
                    && fromFile != null && !(fromFile instanceof android.graphics.drawable.ColorDrawable) && arrays.length == 2;
            detail = "in " + holder.getSimpleName() + ", key " + Long.toHexString(key) + ": " + answered + ", then " + fromFile;
            return ok;
        });
    }

    private void connectivity() {
        step("connectivity", () -> {
            android.content.IntentFilter filter = new android.content.IntentFilter(android.net.ConnectivityManager.CONNECTIVITY_ACTION);
            android.content.Intent sticky = registerReceiver(null, filter);
            android.net.NetworkInfo active = ((android.net.ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE)).getActiveNetworkInfo();
            final android.content.Intent[] returned = new android.content.Intent[1];
            returned[0] = registerReceiver(new android.content.BroadcastReceiver() {
                private int calls;

                @Override
                public void onReceive(Context context, android.content.Intent intent) {
                    calls++;
                    android.net.NetworkInfo info = intent.getParcelableExtra(android.net.ConnectivityManager.EXTRA_NETWORK_INFO);
                    boolean same = android.net.ConnectivityManager.CONNECTIVITY_ACTION.equals(intent.getAction()) && info != null && info.isConnected()
                            && intent.getIntExtra(android.net.ConnectivityManager.EXTRA_NETWORK_TYPE, -1) == info.getType()
                            && !intent.getBooleanExtra(android.net.ConnectivityManager.EXTRA_NO_CONNECTIVITY, false);
                    Log.i("DataTest", "connectivity-received " + (same && calls == 1 && doneLogged && android.os.Looper.myLooper() == android.os.Looper.getMainLooper() ? "ok" : "FAILED"));
                    unregisterReceiver(this);
                }
            }, filter);
            android.net.NetworkInfo info = sticky == null ? null : (android.net.NetworkInfo) sticky.getParcelableExtra(android.net.ConnectivityManager.EXTRA_NETWORK_INFO);
            detail = sticky == null ? "no sticky intent" : "type=" + sticky.getIntExtra(android.net.ConnectivityManager.EXTRA_NETWORK_TYPE, -1) + " info=" + (info != null);
            return sticky != null && returned[0] != null && info != null && active != null && info.getType() == active.getType() && info.isConnected() == active.isConnected()
                    && sticky.getIntExtra(android.net.ConnectivityManager.EXTRA_NETWORK_TYPE, -1) == active.getType()
                    && !sticky.getBooleanExtra(android.net.ConnectivityManager.EXTRA_NO_CONNECTIVITY, false);
        });
    }

    /**
     * The phone's language and region: which they are the test says before the app starts (megler_set_locale) and reads off the line
     * logged here; what the step itself holds is that the ways of asking agree: Locale, LocaleList, the resources' configuration,
     * and the country TelephonyManager says the network and the SIM are in (an app's default country for a phone number).
     */
    private void locale() {
        step("locale", () -> {
            java.util.Locale locale = java.util.Locale.getDefault();
            android.telephony.TelephonyManager tm = (android.telephony.TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
            String network = tm.getNetworkCountryIso(), sim = tm.getSimCountryIso();
            java.util.Locale config = getResources().getConfiguration().getLocales().get(0);
            Log.i("DataTest", "locale is " + locale.toLanguageTag() + " network=" + network + " sim=" + sim + " config=" + config.toLanguageTag());
            detail = locale + " list " + android.os.LocaleList.getDefault() + " config " + config;
            return locale.equals(android.os.LocaleList.getDefault().get(0)) && locale.equals(config)
                    && network.equals(locale.getCountry().toLowerCase(java.util.Locale.ROOT)) && sim.equals(network);
        });
    }

    /**
     * PhoneNumberUtils as Android answers: which characters are a number's (an app's own formatter is fed only those), separators
     * stripped, keypad letters made digits, the network and post-dial portions, a loose comparison of the same number written two
     * ways, and NANP's dashes.
     */
    private void phoneNumber() {
        step("phone-number", () -> {
            String stripped = android.telephony.PhoneNumberUtils.stripSeparators("+7 (912) 345-67-89");
            String normal = android.telephony.PhoneNumberUtils.normalizeNumber("1-800-FLOWERS");
            String network = android.telephony.PhoneNumberUtils.extractNetworkPortion("+1 650,123;4");
            String post = android.telephony.PhoneNumberUtils.extractPostDialPortion("+1 650,123;4");
            android.text.SpannableStringBuilder typed = new android.text.SpannableStringBuilder("6505551234");
            android.telephony.PhoneNumberUtils.formatNumber(typed, android.telephony.PhoneNumberUtils.FORMAT_NANP);
            String nanp = typed.toString();
            boolean same = android.telephony.PhoneNumberUtils.compare("+7 912 345 67 89", "8 (912) 345-67-89");
            boolean other = android.telephony.PhoneNumberUtils.compare("+7 912 345 67 89", "+7 912 345 67 80");
            detail = stripped + " " + normal + " " + network + " " + post + " " + nanp + " same=" + same + " other=" + other;
            return android.telephony.PhoneNumberUtils.isNonSeparator('7') && android.telephony.PhoneNumberUtils.isNonSeparator('+')
                    && !android.telephony.PhoneNumberUtils.isNonSeparator(' ') && !android.telephony.PhoneNumberUtils.isNonSeparator('(')
                    && android.telephony.PhoneNumberUtils.isReallyDialable('#') && !android.telephony.PhoneNumberUtils.isReallyDialable('N')
                    && "+79123456789".equals(stripped) && "18003569377".equals(normal) && "+1650".equals(network) && ",123;4".equals(post)
                    && "650-555-1234".equals(nanp) && same && !other;
        });
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

    /**
     * The key and certificate of the TLS server of the network security configuration's steps: EC P-256, for localhost and
     * 127.0.0.1, serverAuth, issued by the test authority "Megler Test Root CA" (res/raw/trust_test_cas.pem, after "Megler Test
     * Other CA", which issued nothing; alone in res/raw/trust_test_ca.der), all three valid from 2026 to 2126. Made with
     * openssl 3.5: ecparam -genkey for each key; req, then x509 -req -signkey with basicConstraints CA:true for the two
     * authorities, and -CA for the server with CA:false and its two names as subjectAltName. The pins in
     * res/xml/network_security_config.xml are the SHA-256 of each one's key: x509 -pubkey | pkey -pubin -outform DER | dgst -sha256.
     */
    private static final String TRUST_CERT = "MIIB1DCCAXqgAwIBAgIITUVHTEVSAAMwCgYIKoZIzj0EAwIwNTEcMBoGA1UEAwwTTWVnbGVyIFRlc3QgUm9vdCBDQTEVMBMGA1UECgwM"
            + "TWVnbGVyIHRlc3RzMCAXDTI2MDEwMTAwMDAwMFoYDzIxMjYwMTAxMDAwMDAwWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjOPQMB"
            + "BwNCAATMftIn9+WMWAYJ2Ullz8TWBI++crUYxLTJV9h8XjBSRseMzKJITb1gnFKApOyl8T8Q8YedJaINdZnS/PG/Bgnyo4GSMIGPMAwGA1UdEwEB/wQCMAAw"
            + "DgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBoGA1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATAdBgNVHQ4EFgQUz/EiqbIAPGY07vzsM/RL"
            + "0yh+pFowHwYDVR0jBBgwFoAUOJ5HrJ20l/4sBLxKbzLlevgXp5cwCgYIKoZIzj0EAwIDSAAwRQIgcjh7yte3QFMPpx66VoUdkAyu3j3gYA3/3sc4OQdOrA4C"
            + "IQCy6wv6mB/9FlKzDDapaMPKv8cZrsQJXbtwu6e4gp60Jg==";
    private static final String TRUST_KEY = "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg7J/43nN/3nmLHwGjhJgTr9rIJWTiUeSl0++gK2Q6I7ShRANCAATMftIn"
            + "9+WMWAYJ2Ullz8TWBI++crUYxLTJV9h8XjBSRseMzKJITb1gnFKApOyl8T8Q8YedJaINdZnS/PG/Bgny";

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
     * A TLS server of the app's own on 127.0.0.1 and Android's legacy SSLCertificateSocketFactory, which trusts its certificate:
     * the handshake, a line each way, the session's protocol, suite and peer, a host name the certificate does not name refused.
     * Then the same factory with the platform's trust again, which resumes the session its cache keeps, and a factory without
     * a cache, whose platform trust must refuse the same server, and the server must see the handshake fail.
     */
    private boolean tlsLoopback() throws Exception {
        javax.net.ssl.SSLContext[] contexts = tlsContexts();
        final javax.net.ssl.SSLServerSocket server = (javax.net.ssl.SSLServerSocket) contexts[0].getServerSocketFactory()
                .createServerSocket(0, 5, java.net.InetAddress.getByName("127.0.0.1"));
        final String[] heard = new String[3];
        Thread t = new Thread(() -> {
            for (int i = 0; i < 3; i++) {
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
        // The legacy factory is used by both Meta Lite clients, with their own trust managers.
        android.net.SSLCertificateSocketFactory factory = (android.net.SSLCertificateSocketFactory)
                android.net.SSLCertificateSocketFactory.getDefault(10000, new android.net.SSLSessionCache(new java.io.File(getCacheDir(), "tls-sessions")));
        java.security.KeyStore trusted = java.security.KeyStore.getInstance(java.security.KeyStore.getDefaultType());
        trusted.load(null, null);
        trusted.setCertificateEntry("server", tlsCertificate());
        javax.net.ssl.TrustManagerFactory tmf = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
        tmf.init(trusted);
        factory.setTrustManagers(tmf.getTrustManagers());
        if (factory.getSupportedCipherSuites().length == 0 || factory.getDefaultCipherSuites().length == 0)
            throw new AssertionError("legacy TLS cipher suites missing");
        try (java.net.Socket unconnected = factory.createSocket()) {
            factory.setHostname(unconnected, "localhost");
            factory.setUseSessionTickets(unconnected, true);
        }
        java.lang.reflect.Method encode = android.net.SSLCertificateSocketFactory.class.getMethod("toLengthPrefixedList", byte[][].class);
        byte[][] protocols = { "h2".getBytes("US-ASCII"), "http/1.1".getBytes("US-ASCII") };
        byte[] encoded = (byte[])encode.invoke(null, (Object)protocols);
        if (encoded.length != 12 || encoded[0] != 2 || encoded[3] != 8) throw new AssertionError("ALPN encoding");
        try { encode.invoke(null, (Object)new byte[][]{new byte[0]}); throw new AssertionError("empty ALPN accepted"); }
        catch (java.lang.reflect.InvocationTargetException expected) {
            if (!(expected.getCause() instanceof IllegalArgumentException)) throw expected;
        }
        android.net.SSLCertificateSocketFactory.class.getMethod("setAlpnProtocols", byte[][].class).invoke(factory, (Object)protocols);
        boolean wrongHostRefused = false;
        String back, protocol, suite;
        boolean peer;
        Object writeTimeout;
        byte[] first;
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) factory.createSocket("localhost", server.getLocalPort())) {
            s.setSoTimeout(10000);
            s.startHandshake();
            javax.net.ssl.SSLSession session = s.getSession();
            first = session.getId();
            protocol = session.getProtocol();
            suite = session.getCipherSuite();
            peer = session.getPeerCertificates()[0].equals(tlsCertificate());
            factory.setUseSessionTickets(s, true);
            // The write timeout is an option of the socket's descriptor, which a socket has once it is connected: on an
            // unconnected one Android's conscrypt fails with EBADF, and so does this one.
            android.net.SSLCertificateSocketFactory.class.getMethod("setSoWriteTimeout", java.net.Socket.class, int.class).invoke(factory, s, 10000);
            writeTimeout = Class.forName("com.android.org.conscrypt.OpenSSLSocketImpl").getMethod("getSoWriteTimeout").invoke(s);
            try { android.net.SSLCertificateSocketFactory.class.getMethod("verifyHostname", java.net.Socket.class, String.class).invoke(null, s, "wrong.invalid"); }
            catch (java.lang.reflect.InvocationTargetException expected) {
                if (!(expected.getCause() instanceof javax.net.ssl.SSLPeerUnverifiedException)) throw expected;
                wrongHostRefused = true;
            }
            s.getOutputStream().write("ping\n".getBytes("UTF-8"));
            s.getOutputStream().flush();
            back = new java.io.BufferedReader(new java.io.InputStreamReader(s.getInputStream(), "UTF-8")).readLine();
        }
        // Back to the platform's trust: the factory makes a new context, whose session cache is the same SSLSessionCache. A
        // session it keeps is resumed without the certificate being looked at again, as Android's factory does (conscrypt
        // checks a certificate in a full handshake only), so this handshake is made, and with the first session.
        factory.setTrustManagers(null);
        String resumed;
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) factory.createSocket("localhost", server.getLocalPort())) {
            resumed = java.util.Arrays.equals(first, s.getSession().getId()) ? "the first session" : "another session";
        } catch (javax.net.ssl.SSLHandshakeException e) {
            resumed = "SSLHandshakeException";
        }
        // A factory without a cache has a full handshake to make: once its trust is set back to the platform's, the context
        // made with the app's trust is not used again, and the platform's must refuse the server.
        android.net.SSLCertificateSocketFactory fresh = (android.net.SSLCertificateSocketFactory) android.net.SSLCertificateSocketFactory.getDefault(10000, null);
        fresh.setTrustManagers(tmf.getTrustManagers());
        fresh.getDefaultCipherSuites();
        fresh.setTrustManagers(null);
        String refused;
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) fresh.createSocket("localhost", server.getLocalPort())) {
            s.setSoTimeout(10000);
            s.startHandshake();
            refused = "handshake made";
        } catch (javax.net.ssl.SSLHandshakeException e) {
            refused = "SSLHandshakeException";
        }
        t.join(10000);
        server.close();
        detail = "legacy-factory wrongHostRefused=" + wrongHostRefused + " writeTimeout=" + writeTimeout + " protocol=" + protocol + " suite=" + suite + " peer=" + peer + " heard=" + heard[0] + " back=" + back
                + " cached=" + resumed + " platform=" + refused + " then " + heard[2];
        Log.i("DataTest", "tls-loopback-said " + detail);
        return !t.isAlive() && wrongHostRefused && Integer.valueOf(10000).equals(writeTimeout) && "TLSv1.2".equals(protocol) && suite.startsWith("TLS_ECDHE_ECDSA_") && peer && "ping".equals(heard[0]) && "pong".equals(back)
                && "the first session".equals(resumed) && "SSLHandshakeException".equals(refused) && heard[2] != null && heard[2].startsWith("the server: ");
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
                o.write("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\nConnection: close\r\n\r\nmegler!".getBytes("ISO-8859-1"));
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
     * The app's network security configuration for TLS (res/xml/network_security_config.xml), as Android 9 has it: an authority
     * of the app's own trusted for the names it gives it, as Ozon trusts the Russian Trusted Root CA. A TLS server of the app's
     * on 127.0.0.1 has a certificate that authority issued; each connection to it gives the server a name, the peer's, which
     * chooses the configuration. Without TLS (no conscrypt) the steps hold, as there is nothing to connect with.
     */
    private void networkTrust() {
        step("network-trust", () -> !conscrypt || offMain(this::networkTrustDefault));
        step("network-trust-domains", () -> !conscrypt || offMain(this::networkTrustDomains));
        step("network-trust-pins", () -> !conscrypt || offMain(this::networkTrustPins));
    }

    /**
     * The configuration's provider first among the security providers, and the platform's trust manager the configuration's
     * (RootTrustManager). localhost trusted through Android's HttpsURLConnection with its default socket factory, and through a
     * context of the app's own over the default TrustManagerFactory; the host-aware check (X509TrustManagerExtensions, as
     * OkHttp's chain cleaner makes it) gives the chain ending in the authority. 127.0.0.1, which only the base configuration
     * names (Android's authorities, and no debug-overrides for an app that is not debuggable), refused. Two names of one
     * configuration are trusted alike, localhost and 127.0.0.1 are not (isSameTrustConfiguration). And a KeyStore given to the
     * factory is a configuration of its own: its certificate trusted for every host.
     */
    private boolean networkTrustDefault() throws Exception {
        String first = java.security.Security.getProviders()[0].getName();
        javax.net.ssl.TrustManagerFactory tmf = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
        tmf.init((java.security.KeyStore) null);
        javax.net.ssl.X509TrustManager platform = (javax.net.ssl.X509TrustManager) tmf.getTrustManagers()[0];
        java.security.cert.X509Certificate leaf = trustCertificate(), authority = trustAuthority();
        javax.net.ssl.SSLServerSocket server = trustServer();
        try {
            javax.net.ssl.HttpsURLConnection c = (javax.net.ssl.HttpsURLConnection) new java.net.URL("https://localhost:" + server.getLocalPort() + "/trust").openConnection();
            c.setConnectTimeout(5000);
            c.setReadTimeout(10000);
            String https;
            try {
                https = c.getResponseCode() + " " + readAll(c.getInputStream());
            } catch (java.io.IOException e) {
                https = "failed: " + rootMessage(e);
            } finally {
                c.disconnect();
            }
            javax.net.ssl.SSLContext own = javax.net.ssl.SSLContext.getInstance("TLS");
            own.init(null, tmf.getTrustManagers(), null);
            String byOwn = trustAs(server, own.getSocketFactory(), "localhost");
            String byAddress = trustAs(server, (javax.net.ssl.SSLSocketFactory) javax.net.ssl.SSLSocketFactory.getDefault(), "127.0.0.1");
            android.net.http.X509TrustManagerExtensions extensions = new android.net.http.X509TrustManagerExtensions(platform);
            java.security.cert.X509Certificate[] served = { leaf };
            java.util.List<java.security.cert.X509Certificate> chain = extensions.checkServerTrusted(served, "ECDHE_ECDSA", "localhost");
            String elsewhere;
            try {
                extensions.checkServerTrusted(served, "ECDHE_ECDSA", "127.0.0.1");
                elsewhere = "trusted";
            } catch (java.security.cert.CertificateException e) {
                elsewhere = "refused";
            }
            boolean sameSub = extensions.isSameTrustConfiguration("a.sub.megler.test", "sub.megler.test");
            boolean sameAddress = extensions.isSameTrustConfiguration("localhost", "127.0.0.1");
            java.security.KeyStore store = java.security.KeyStore.getInstance(java.security.KeyStore.getDefaultType());
            store.load(null, null);
            store.setCertificateEntry("authority", authority);
            javax.net.ssl.TrustManagerFactory stored = javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
            stored.init(store);
            javax.net.ssl.X509TrustManager storedTrust = (javax.net.ssl.X509TrustManager) stored.getTrustManagers()[0];
            String byStore;
            try {
                storedTrust.checkServerTrusted(served, "ECDHE_ECDSA");
                byStore = "trusted";
            } catch (java.security.cert.CertificateException e) {
                byStore = "refused: " + rootMessage(e);
            }
            detail = "first=" + first + " trust=" + platform.getClass().getName() + " https=" + https + " own=" + byOwn + " address=" + byAddress
                    + " chain=" + chain.size() + " ending " + chain.get(chain.size() - 1).getSubjectX500Principal().getName() + " elsewhere=" + elsewhere
                    + " same=" + sameSub + "/" + sameAddress + " store=" + byStore + " " + storedTrust.getAcceptedIssuers().length;
            Log.i("DataTest", "network-trust-said " + detail);
            return "AndroidNSSP".equals(first) && "android.security.net.config.RootTrustManager".equals(platform.getClass().getName())
                    && "200 trusted!".equals(https) && "trusted".equals(byOwn) && byAddress.startsWith("refused: Trust anchor for certification path not found")
                    && chain.size() == 2 && chain.get(1).equals(authority) && "refused".equals(elsewhere) && sameSub && !sameAddress
                    && "trusted".equals(byStore) && storedTrust.getAcceptedIssuers().length == 1;
        } finally {
            server.close();
        }
    }

    /**
     * Names a <domain> covers: sub.megler.test itself and two levels under it (includeSubdomains), whose anchor is the authority
     * read from DER; nested.sub.megler.test, a <domain-config> inside that one without anchors of its own, which takes its
     * parent's; localhost written in another case. megler.test, above them, refused.
     */
    private boolean networkTrustDomains() throws Exception {
        javax.net.ssl.SSLServerSocket server = trustServer();
        try {
            javax.net.ssl.SSLSocketFactory f = (javax.net.ssl.SSLSocketFactory) javax.net.ssl.SSLSocketFactory.getDefault();
            String sub = trustAs(server, f, "sub.megler.test"), deep = trustAs(server, f, "a.b.sub.megler.test"), nested = trustAs(server, f, "nested.sub.megler.test"),
                    cased = trustAs(server, f, "LocalHost"), above = trustAs(server, f, "megler.test");
            detail = "sub=" + sub + " deep=" + deep + " nested=" + nested + " cased=" + cased + " above=" + above;
            Log.i("DataTest", "network-trust-domains-said " + detail);
            return "trusted".equals(sub) && "trusted".equals(deep) && "trusted".equals(nested) && "trusted".equals(cased)
                    && above.startsWith("refused: Trust anchor for certification path not found");
        } finally {
            server.close();
        }
    }

    /**
     * Pins: the authority's key pinned, trusted; only a key the chain does not have pinned, refused; that pin expired, trusted;
     * the authority's anchor overriding pins, trusted; the server's own key pinned, trusted.
     */
    private boolean networkTrustPins() throws Exception {
        javax.net.ssl.SSLServerSocket server = trustServer();
        try {
            javax.net.ssl.SSLSocketFactory f = (javax.net.ssl.SSLSocketFactory) javax.net.ssl.SSLSocketFactory.getDefault();
            String pinned = trustAs(server, f, "pinned.sub.megler.test"), bad = trustAs(server, f, "badpin.megler.test"),
                    expired = trustAs(server, f, "expired.megler.test"), overridden = trustAs(server, f, "override.megler.test"),
                    leafPin = trustAs(server, f, "leafpin.megler.test");
            detail = "pinned=" + pinned + " bad=" + bad + " expired=" + expired + " overridden=" + overridden + " leaf=" + leafPin;
            Log.i("DataTest", "network-trust-pins-said " + detail);
            return "trusted".equals(pinned) && "refused: Pin verification failed".equals(bad) && "trusted".equals(expired)
                    && "trusted".equals(overridden) && "trusted".equals(leafPin);
        } finally {
            server.close();
        }
    }

    private static java.security.cert.X509Certificate trustCertificate() throws Exception {
        return (java.security.cert.X509Certificate) java.security.cert.CertificateFactory.getInstance("X.509")
                .generateCertificate(new java.io.ByteArrayInputStream(java.util.Base64.getDecoder().decode(TRUST_CERT)));
    }

    /** The test authority, as the app's DER resource has it. */
    private java.security.cert.X509Certificate trustAuthority() throws Exception {
        try (java.io.InputStream in = getResources().openRawResource(R.raw.trust_test_ca)) {
            return (java.security.cert.X509Certificate) java.security.cert.CertificateFactory.getInstance("X.509").generateCertificate(in);
        }
    }

    /** The TLS server of the network security configuration's steps, on 127.0.0.1 until it is closed: an HTTP GET is answered
     *  "trusted!", any other line "pong"; a client that refuses its certificate ends that connection alone. */
    private static javax.net.ssl.SSLServerSocket trustServer() throws Exception {
        java.security.PrivateKey key = java.security.KeyFactory.getInstance("EC").generatePrivate(new java.security.spec.PKCS8EncodedKeySpec(java.util.Base64.getDecoder().decode(TRUST_KEY)));
        java.security.KeyStore keys = java.security.KeyStore.getInstance(java.security.KeyStore.getDefaultType());
        keys.load(null, null);
        keys.setKeyEntry("server", key, new char[0], new java.security.cert.Certificate[] { trustCertificate() });
        javax.net.ssl.KeyManagerFactory kmf = javax.net.ssl.KeyManagerFactory.getInstance(javax.net.ssl.KeyManagerFactory.getDefaultAlgorithm());
        kmf.init(keys, new char[0]);
        javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
        context.init(kmf.getKeyManagers(), null, null);
        final javax.net.ssl.SSLServerSocket server = (javax.net.ssl.SSLServerSocket) context.getServerSocketFactory()
                .createServerSocket(0, 16, java.net.InetAddress.getByName("127.0.0.1"));
        Thread t = new Thread(() -> {
            while (!server.isClosed()) {
                try (java.net.Socket c = server.accept()) {
                    c.setSoTimeout(10000);
                    java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(c.getInputStream(), "ISO-8859-1"));
                    String line = r.readLine();
                    java.io.OutputStream o = c.getOutputStream();
                    if (line != null && line.startsWith("GET ")) {
                        for (String h; (h = r.readLine()) != null && !h.isEmpty();) { }
                        o.write("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\ntrusted!".getBytes("ISO-8859-1"));
                    } else {
                        o.write("pong\n".getBytes("ISO-8859-1"));
                    }
                    o.flush();
                } catch (java.io.IOException e) {
                    // a client that refused the certificate, or the server closed
                }
            }
        }, "trust-server");
        t.setDaemon(true);
        t.start();
        return server;
    }

    /**
     * A connection to the trust server under the name `host`: a socket to 127.0.0.1 with TLS laid over it for that name, the
     * peer's. "trusted" when the handshake is made and the server answers, else what refused it.
     */
    private static String trustAs(javax.net.ssl.SSLServerSocket server, javax.net.ssl.SSLSocketFactory factory, String host) throws java.io.IOException {
        java.net.Socket plain = new java.net.Socket();
        plain.connect(new java.net.InetSocketAddress(java.net.InetAddress.getByName("127.0.0.1"), server.getLocalPort()), 5000);
        try (javax.net.ssl.SSLSocket s = (javax.net.ssl.SSLSocket) factory.createSocket(plain, host, server.getLocalPort(), true)) {
            s.setSoTimeout(10000);
            s.startHandshake();
            s.getOutputStream().write("ping\n".getBytes("UTF-8"));
            s.getOutputStream().flush();
            String back = new java.io.BufferedReader(new java.io.InputStreamReader(s.getInputStream(), "UTF-8")).readLine();
            return "pong".equals(back) ? "trusted" : "answered " + back;
        } catch (javax.net.ssl.SSLHandshakeException e) {
            return "refused: " + rootMessage(e);
        }
    }

    /** The message of what was first thrown: a refused certificate's reason under the handshake's exception. */
    private static String rootMessage(Throwable t) {
        while (t.getCause() != null && t.getCause() != t) t = t.getCause();
        return t.getMessage();
    }

    /**
     * Android's key store as Amazon's sign-in library (in MX Player) uses it: an RSA pair made under an alias by
     * KeyPairGenerator.getInstance("RSA", "AndroidKeyStore"), a key of sixteen bytes wrapped with its public key and unwrapped with
     * its private one, both found again through KeyStore.getInstance("AndroidKeyStore"); an EC pair the same way, which signs; an
     * AES key, as Tink's master key for EncryptedSharedPreferences, which encrypts; and all three are there for the next run, which
     * finds them before it makes them again (an EC pair once made the whole file unreadable at the next start, package 560).
     */
    private void keystore(final long run) {
        step("keystore", () -> {
            java.security.KeyStore store = java.security.KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            boolean before = store.containsAlias("megler-test-rsa"), beforeEc = store.containsAlias("megler-test-ec"),
                    beforeAes = store.containsAlias("megler-test-aes");
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

            if (!beforeAes) {
                javax.crypto.KeyGenerator aes = javax.crypto.KeyGenerator.getInstance("AES", "AndroidKeyStore");
                aes.init(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-aes", android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                        | android.security.keystore.KeyProperties.PURPOSE_DECRYPT).setKeySize(256)
                        .setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE).build());
                aes.generateKey();
            }
            javax.crypto.SecretKey aesKey = (javax.crypto.SecretKey) store.getKey("megler-test-aes", null);
            javax.crypto.Cipher gcm = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
            gcm.init(javax.crypto.Cipher.ENCRYPT_MODE, aesKey);
            byte[] sealed = gcm.doFinal(secret);
            javax.crypto.Cipher open = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
            open.init(javax.crypto.Cipher.DECRYPT_MODE, aesKey, gcm.getParameters());
            boolean aesHeld = aesKey != null && aesKey.getEncoded().length == 32 && Arrays.equals(secret, open.doFinal(sealed));
            detail = "rsa=" + rsaHeld + " (" + bits + " bits, " + wrapped.length + " bytes wrapped) ec=" + ecHeld + " (" + ecBits + " bits) aes=" + aesHeld
                    + " there-before=" + before + "," + beforeEc + "," + beforeAes + " run=" + run;
            Log.i("DataTest", "keystore " + detail);
            return rsaHeld && ecHeld && aesHeld && before == (run > 1) && beforeEc == (run > 1) && beforeAes == (run > 1);
        });
    }

    /**
     * Path.op as views round their corners: a frame's rectangle less its rounded rectangle, painted with DST_OUT over the
     * content in a layer, takes away the corners alone (TikTok Lite's frames: op left the whole rectangle and the content
     * went with it, package 624); a rectangle's intersection with an oval within it is the oval; two overlapping squares'
     * XOR leaves out where they overlap.
     */
    private void pathOps() {
        step("path-op", () -> {
            android.graphics.Paint red = new android.graphics.Paint();
            red.setColor(0xFFFF0000);
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(100, 100, android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.Canvas c = new android.graphics.Canvas(bm);
            int saved = c.saveLayer(new android.graphics.RectF(0, 0, 100, 100), null, android.graphics.Canvas.ALL_SAVE_FLAG);
            c.drawRect(0, 0, 100, 100, red);
            android.graphics.Path round = new android.graphics.Path();
            round.addRoundRect(new android.graphics.RectF(0, 0, 100, 100), new float[] { 30, 30, 30, 30, 30, 30, 30, 30 }, android.graphics.Path.Direction.CW);
            android.graphics.Path corners = new android.graphics.Path();
            corners.addRect(0, 0, 100, 100, android.graphics.Path.Direction.CW);
            boolean difference = corners.op(round, android.graphics.Path.Op.DIFFERENCE);
            android.graphics.Paint away = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            away.setColor(0xFFFFFFFF);
            away.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.DST_OUT));
            c.drawPath(corners, away);
            c.restoreToCount(saved);
            int centre = bm.getPixel(50, 50), corner = bm.getPixel(1, 1), edge = bm.getPixel(50, 1);

            android.graphics.Bitmap b2 = android.graphics.Bitmap.createBitmap(100, 100, android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.Path square = new android.graphics.Path();
            square.addRect(0, 0, 100, 100, android.graphics.Path.Direction.CW);
            android.graphics.Path oval = new android.graphics.Path();
            oval.addOval(new android.graphics.RectF(20, 20, 80, 80), android.graphics.Path.Direction.CW);
            boolean intersect = square.op(oval, android.graphics.Path.Op.INTERSECT);
            new android.graphics.Canvas(b2).drawPath(square, red);
            int inOval = b2.getPixel(50, 50), outOval = b2.getPixel(5, 5);

            android.graphics.Bitmap b3 = android.graphics.Bitmap.createBitmap(100, 100, android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.Path x = new android.graphics.Path(), y = new android.graphics.Path();
            x.addRect(0, 0, 60, 60, android.graphics.Path.Direction.CW);
            y.addRect(40, 40, 100, 100, android.graphics.Path.Direction.CW);
            boolean xor = x.op(y, android.graphics.Path.Op.XOR);
            new android.graphics.Canvas(b3).drawPath(x, red);
            int overlap = b3.getPixel(50, 50), onlyX = b3.getPixel(10, 10), onlyY = b3.getPixel(90, 90);

            detail = String.format("difference=%b centre=%08x corner=%08x edge=%08x intersect=%b in=%08x out=%08x xor=%b overlap=%08x x=%08x y=%08x",
                    difference, centre, corner, edge, intersect, inOval, outOval, xor, overlap, onlyX, onlyY);
            Log.i("DataTest", "path-op " + detail);
            return difference && centre == 0xFFFF0000 && (corner >>> 24) == 0 && edge == 0xFFFF0000
                    && intersect && inOval == 0xFFFF0000 && (outOval >>> 24) == 0
                    && xor && (overlap >>> 24) == 0 && onlyX == 0xFFFF0000 && onlyY == 0xFFFF0000;
        });
    }

    /**
     * Paint.getTextPath as Google Maps' legacy renderer draws the labels of its map with it: one Path handed over label after
     * label, stroked in the halo's colour and then filled. The path holds the glyphs alone each time, where drawText would put
     * them -- the stand-in's glyphs are boxes half an em wide on advances of 0.6 em (mg_gfx_standin.c), so there is ground
     * between two letters -- whichever of the two methods is called, and with the alignment. Package 684 added the run's whole
     * box to the path at each call, and the labels were grey boxes.
     */
    private void textPath() {
        step("text-path", () -> {
            android.graphics.Paint fill = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            fill.setTextSize(40);
            fill.setColor(0xFFFF0000);
            android.graphics.Paint halo = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            halo.setStyle(android.graphics.Paint.Style.STROKE);
            halo.setStrokeWidth(2);
            halo.setColor(0xFF0000FF);
            android.graphics.Path path = new android.graphics.Path();
            android.graphics.RectF first = new android.graphics.RectF(), again = new android.graphics.RectF(), centred = new android.graphics.RectF();
            fill.getTextPath("HI", 0, 2, 10, 50, path);
            path.computeBounds(first, true);
            fill.getTextPath("xHI".toCharArray(), 1, 2, 10, 50, path);
            path.computeBounds(again, true);
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(80, 60, android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.Canvas c = new android.graphics.Canvas(bm);
            c.drawPath(path, halo);
            c.drawPath(path, fill);
            int inH = bm.getPixel(20, 40), between = bm.getPixel(32, 40), edge = bm.getPixel(30, 40), inI = bm.getPixel(44, 40), above = bm.getPixel(20, 18);
            fill.setTextAlign(android.graphics.Paint.Align.CENTER);
            fill.getTextPath("HI", 0, 2, 34, 50, path);
            path.computeBounds(centred, true);
            fill.getTextPath("", 0, 0, 34, 50, path);
            boolean emptied = path.isEmpty(), refused = false;
            try {
                fill.getTextPath("HI", 1, 5, 0, 0, path);
            } catch (IndexOutOfBoundsException e) {
                refused = true;
            }
            android.graphics.RectF want = new android.graphics.RectF(10, 22, 54, 50);
            detail = String.format("first=%s again=%s centred=%s H=%08x between=%08x edge=%08x I=%08x above=%08x emptied=%b refused=%b", first, again, centred,
                    inH, between, edge, inI, above, emptied, refused);
            Log.i("DataTest", "text-path " + detail);
            return first.equals(want) && again.equals(want) && centred.equals(want) && inH == 0xFFFF0000 && (between >>> 24) == 0 && edge == 0xFF0000FF
                    && inI == 0xFFFF0000 && (above >>> 24) == 0 && emptied && refused;
        });
    }

    /** The harness's F12 (mg_java_tests.c, run_data): the main thread made to stand in Object.wait (stuckMain); F11: a translucent
        activity over this one (Clear). */
    @Override public boolean onKeyDown(int keyCode, android.view.KeyEvent event) {
        if (keyCode == android.view.KeyEvent.KEYCODE_F12) {
            stuckMain();
            return true;
        }
        if (keyCode == android.view.KeyEvent.KEYCODE_F11) {
            sMain = this;
            sStoppedUnderClear = false;
            sWatchStop = true;
            startActivity(new android.content.Intent(this, Clear.class));
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    /*
     * A translucent activity over this one stays over it with this one visible: paused, not stopped, its window drawn below --
     * as TikTok Lite's consent dialog (a translucent activity, not a floating one) is over its feed. Megler stopped the activity
     * below one whose window was not floating, and took its window away: the screen behind the consent showed black, and
     * stayed black once the consent's own view had gone (package 619).
     */
    static volatile Main sMain;
    static volatile boolean sWatchStop, sStoppedUnderClear;
    @Override protected void onStop() {
        super.onStop();
        if (sWatchStop) sStoppedUnderClear = true;
    }
    public static class Clear extends Activity {
        @Override protected void onResume() {
            super.onResume();
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                Main below = sMain;
                boolean attached = below != null && below.getWindow().getDecorView().isAttachedToWindow();
                Log.i("DataTest", "translucent-over " + (!sStoppedUnderClear && attached ? "ok" : "FAILED") + " stopped=" + sStoppedUnderClear + " attached=" + attached);
                sWatchStop = false;
                finish();
            }, 500);
        }
    }

    /**
     * The main thread waits 8 s on a lock that another thread notifies then: the main thread's sampler says its stack meanwhile,
     * this method in it (mg_dvm_stuck_say), where its looper's idle wait is not said.
     */
    private void stuckMain() {
        final Object lock = new Object();
        final boolean[] done = new boolean[1];
        new Thread(() -> {
            try { Thread.sleep(8000); } catch (InterruptedException e) { }
            synchronized (lock) {
                done[0] = true;
                lock.notifyAll();
            }
        }, "stuck-releaser").start();
        long started = android.os.SystemClock.uptimeMillis();
        synchronized (lock) {
            while (!done[0]) {
                try { lock.wait(); } catch (InterruptedException e) { break; }
            }
        }
        Log.i("DataTest", "stuck-main ok waited=" + (android.os.SystemClock.uptimeMillis() - started) / 1000 + " s");
    }

    /**
     * A key no provider takes (a secret key with no encoding), for a transformation that AndroidKeyStoreBCWorkaround has too:
     * Cipher.init ends at once in the first provider's exception, as on Android (conscrypt's NullPointerException for no
     * encoding). That provider's cipher, the last one tried, chose a cipher again among them all, itself too, until the stack was
     * full: Google Play services' mobstore key went 5026 frames deep (619).
     */
    private void keystoreForeignKey() {
        step("keystore-foreign-key", () -> {
            javax.crypto.SecretKey odd = new javax.crypto.SecretKey() {
                public String getAlgorithm() { return "AES"; }
                public String getFormat() { return null; }
                public byte[] getEncoded() { return null; }
            };
            javax.crypto.Cipher gcm = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
            String said;
            try {
                gcm.init(javax.crypto.Cipher.ENCRYPT_MODE, odd);
                said = "taken";
            } catch (StackOverflowError e) {
                said = "StackOverflowError";
            } catch (Exception e) {
                said = e.getClass().getName();
            }
            detail = said;
            Log.i("DataTest", "keystore-foreign-key " + detail);
            return !"taken".equals(said) && !"StackOverflowError".equals(said);
        });
    }

    /**
     * The certificates of Android's key store's key pairs, as Google Play services' device registration and many apps read them: an
     * X509Certificate (which they cast to), self-signed with the pair's key, with the spec's subject, serial number and dates (one
     * after 2049, a GeneralizedTime) or Android's own (CN=fake, 1, 1970 to 2048), kept for the next run as it was made; a key set with
     * a chain keeps that chain; and a pair asked for with an attestation challenge is refused with a ProviderException and not kept,
     * as by a device whose key store cannot attest (Google Play services goes on without it; given a certificate that was not
     * X.509, its check-in ended in a ClassCastException, package 573).
     */
    private void keystoreCertificates(final long run) {
        step("keystore-certificates", () -> {
            java.security.KeyStore store = java.security.KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            // The pair the last run made, read before this run makes it again.
            java.security.cert.Certificate kept = store.getCertificate("megler-test-cert-ec");
            String keptSubject = kept instanceof java.security.cert.X509Certificate
                    ? ((java.security.cert.X509Certificate) kept).getSubjectX500Principal().getName() : String.valueOf(kept);
            java.security.KeyPairGenerator ec = java.security.KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
            ec.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-cert-ec", android.security.keystore.KeyProperties.PURPOSE_SIGN)
                    .setDigests(android.security.keystore.KeyProperties.DIGEST_SHA256)
                    .setCertificateSubject(new javax.security.auth.x500.X500Principal("CN=Megler test, O=Megler"))
                    .setCertificateSerialNumber(java.math.BigInteger.valueOf(7)).setCertificateNotBefore(new java.util.Date(1700000000000L))
                    .setCertificateNotAfter(new java.util.Date(4102444800000L)).build());
            java.security.KeyPair pair = ec.generateKeyPair();
            java.security.cert.X509Certificate cert = (java.security.cert.X509Certificate) store.getCertificate("megler-test-cert-ec");
            cert.verify(pair.getPublic());
            java.security.cert.Certificate[] chain = store.getCertificateChain("megler-test-cert-ec");
            boolean ecCert = "CN=Megler test,O=Megler".equals(cert.getSubjectX500Principal().getName())
                    && cert.getIssuerX500Principal().equals(cert.getSubjectX500Principal()) && cert.getSerialNumber().intValue() == 7
                    && cert.getNotBefore().getTime() == 1700000000000L && cert.getNotAfter().getTime() == 4102444800000L
                    && Arrays.equals(cert.getPublicKey().getEncoded(), pair.getPublic().getEncoded()) && "SHA256withECDSA".equalsIgnoreCase(cert.getSigAlgName())
                    && chain.length == 1 && Arrays.equals(chain[0].getEncoded(), cert.getEncoded());

            java.security.KeyPairGenerator rsa = java.security.KeyPairGenerator.getInstance("RSA", "AndroidKeyStore");
            rsa.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-cert-rsa", android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                    | android.security.keystore.KeyProperties.PURPOSE_DECRYPT).setKeySize(2048).build());
            java.security.KeyPair rsaPair = rsa.generateKeyPair();
            java.security.cert.X509Certificate rsaCert = (java.security.cert.X509Certificate) store.getCertificate("megler-test-cert-rsa");
            rsaCert.verify(rsaPair.getPublic());
            boolean rsaDefaults = "CN=fake".equals(rsaCert.getSubjectX500Principal().getName()) && rsaCert.getSerialNumber().intValue() == 1
                    && rsaCert.getNotBefore().getTime() == 0L && rsaCert.getNotAfter().getTime() == 2461449600000L
                    && "SHA256withRSA".equalsIgnoreCase(rsaCert.getSigAlgName());

            store.setKeyEntry("megler-test-cert-set", rsaPair.getPrivate(), null, new java.security.cert.Certificate[] { rsaCert });
            boolean set = Arrays.equals(store.getCertificate("megler-test-cert-set").getEncoded(), rsaCert.getEncoded())
                    && store.getEntry("megler-test-cert-set", null) instanceof java.security.KeyStore.PrivateKeyEntry;
            store.deleteEntry("megler-test-cert-set");

            String attested;
            try {
                java.security.KeyPairGenerator attest = java.security.KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
                attest.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("megler-test-attested", android.security.keystore.KeyProperties.PURPOSE_SIGN)
                        .setDigests(android.security.keystore.KeyProperties.DIGEST_SHA256).setAttestationChallenge(new byte[] { 1, 2, 3 }).build());
                attest.generateKeyPair();
                attested = "made";
            } catch (java.security.ProviderException e) {
                attested = "ProviderException";
            }
            boolean refused = "ProviderException".equals(attested) && !store.containsAlias("megler-test-attested");
            detail = "ec=" + ecCert + " rsa-defaults=" + rsaDefaults + " set=" + set + " attested=" + attested + " kept=" + keptSubject + " run=" + run;
            Log.i("DataTest", "keystore-certificates " + detail);
            return ecCert && rsaDefaults && set && refused && (run > 1 ? "CN=Megler test,O=Megler".equals(keptSubject) : kept == null);
        });
    }

    /**
     * android.util.Base64OutputStream and Base64InputStream, as Sberbank uses one under a DeflaterOutputStream: what
     * goes through the one is what Base64.encode makes, written a piece and a byte at a time, with the default flags (lines of
     * 76 and a line end) and URL_SAFE | NO_WRAP | NO_PADDING; and compressed and encoded, read back the other way (both were
     * stubs that wrote and read nothing, package 576).
     */
    /**
     * java.io's PipedInputStream and PipedOutputStream between two threads, as Play Store hands a download from the network
     * to its file: 4 MB written in pieces of 8 KB through a pipe of 64 KB and read on another thread, with no flush. Its
     * waits are Object.wait(1000) woken by notifyAll: a wake that did not come at once would hold each pipeful up to a
     * second. The time is said, and must be short.
     */
    private void pipedStreams() {
        step("piped-streams", () -> {
            final java.io.PipedInputStream in = new java.io.PipedInputStream(65536);
            final java.io.PipedOutputStream out = new java.io.PipedOutputStream(in);
            final byte[] piece = new byte[8192];
            final int pieces = 512;
            for (int i = 0; i < piece.length; i++) piece[i] = (byte) i;
            final Throwable[] failed = new Throwable[1];
            Thread writer = new Thread(() -> {
                try {
                    for (int i = 0; i < pieces; i++) out.write(piece);
                    out.close();
                } catch (Throwable t) {
                    failed[0] = t;
                }
            }, "pipe-writer");
            long start = System.nanoTime();
            writer.start();
            byte[] buf = new byte[16384];
            long total = 0, sum = 0;
            for (int n; (n = in.read(buf)) > 0;) {
                total += n;
                sum += buf[n - 1];
            }
            writer.join();
            long ms = (System.nanoTime() - start) / 1000000L;
            detail = total + " bytes in " + ms + " ms" + (failed[0] != null ? ", the writer: " + failed[0] : "");
            return failed[0] == null && total == (long) pieces * piece.length && ms < 20000;
        });
    }

    private void base64Streams() {
        step("base64-streams", () -> {
            byte[] data = new byte[1000];
            new java.util.Random(7).nextBytes(data);
            java.io.ByteArrayOutputStream plain = new java.io.ByteArrayOutputStream();
            try (android.util.Base64OutputStream b = new android.util.Base64OutputStream(plain, android.util.Base64.DEFAULT)) {
                b.write(data, 0, 500);
                for (int i = 500; i < data.length; i++) b.write(data[i]);
            }
            boolean encoded = Arrays.equals(plain.toByteArray(), android.util.Base64.encode(data, android.util.Base64.DEFAULT));
            int urlFlags = android.util.Base64.URL_SAFE | android.util.Base64.NO_WRAP | android.util.Base64.NO_PADDING;
            java.io.ByteArrayOutputStream url = new java.io.ByteArrayOutputStream();
            try (android.util.Base64OutputStream b = new android.util.Base64OutputStream(url, urlFlags)) {
                b.write(data, 0, 998);
            }
            boolean urlSafe = Arrays.equals(url.toByteArray(), android.util.Base64.encode(data, 0, 998, urlFlags));
            java.io.ByteArrayOutputStream packed = new java.io.ByteArrayOutputStream();
            try (java.util.zip.DeflaterOutputStream d = new java.util.zip.DeflaterOutputStream(new android.util.Base64OutputStream(packed, android.util.Base64.NO_WRAP))) {
                d.write(data);
            }
            java.io.ByteArrayOutputStream back = new java.io.ByteArrayOutputStream();
            try (java.io.InputStream in = new java.util.zip.InflaterInputStream(new android.util.Base64InputStream(
                    new java.io.ByteArrayInputStream(packed.toByteArray()), android.util.Base64.NO_WRAP))) {
                byte[] buf = new byte[300];
                for (int n; (n = in.read(buf)) > 0;) back.write(buf, 0, n);
            }
            boolean round = Arrays.equals(back.toByteArray(), data);
            detail = "encoded=" + encoded + " url-safe=" + urlSafe + " round=" + round + " " + plain.size() + " characters, " + packed.size() + " packed";
            return encoded && urlSafe && round;
        });
    }

    /**
     * The decoders an app can have: those the shell has, which under the test runner is its stand-in's H.264, AAC and MP3
     * and not H.265 (core/tests/mg_codec_standin.c). They are listed every way the list is asked for, one is found for a
     * film's format, one is made by its type, and asking for H.265 is the IOException Android throws when there is none.
     */
    private void codecs() {
        step("codecs", () -> {
            int count = android.media.MediaCodecList.getCodecCount();
            android.media.MediaCodecList list = new android.media.MediaCodecList(android.media.MediaCodecList.ALL_CODECS);
            StringBuilder names = new StringBuilder();
            for (android.media.MediaCodecInfo i : list.getCodecInfos()) names.append(i.getName()).append(' ');
            String found = list.findDecoderForFormat(android.media.MediaFormat.createVideoFormat("video/avc", 640, 360));
            String made, hevc;
            try {
                android.media.MediaCodec.createDecoderByType("video/avc").release();
                made = "made";
            } catch (java.io.IOException e) {
                made = "IOException";
            }
            try {
                android.media.MediaCodec.createDecoderByType("video/hevc").release();
                hevc = "made";
            } catch (java.io.IOException e) {
                hevc = "IOException";
            }
            detail = "count=" + count + " " + names + "avc=" + found + " create=" + made + " hevc=" + hevc;
            return count == 3 && "c2.megler.avc.decoder c2.megler.aac.decoder c2.megler.mp3.decoder ".equals(names.toString()) && "c2.megler.avc.decoder".equals(found)
                    && "made".equals(made) && "IOException".equals(hevc);
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
     * A ParcelFileDescriptor through a Parcel, as an AIDL stub passes one (microG's People service hands Play Store the
     * account's picture so): written with writeToParcel and read with CREATOR, it reads the file after the writer closed its own.
     */
    private void pfdParcel() {
        step("pfd-parcel", () -> {
            java.io.File file = new java.io.File(getCacheDir(), "pfd-parcel.txt");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(file)) { out.write("avatar".getBytes("UTF-8")); }
            android.os.ParcelFileDescriptor sent = android.os.ParcelFileDescriptor.open(file, android.os.ParcelFileDescriptor.MODE_READ_ONLY);
            android.os.Parcel p = android.os.Parcel.obtain();
            p.writeInt(1);
            sent.writeToParcel(p, 0);
            sent.close();
            p.setDataPosition(0);
            boolean present = p.readInt() != 0;
            android.os.ParcelFileDescriptor got = android.os.ParcelFileDescriptor.CREATOR.createFromParcel(p);
            p.recycle();
            String text = null;
            if (got != null) {
                try (java.io.InputStream in = new android.os.ParcelFileDescriptor.AutoCloseInputStream(got)) {
                    byte[] b = new byte[16];
                    int n = in.read(b);
                    text = new String(b, 0, Math.max(n, 0), "UTF-8");
                }
            }
            detail = "present=" + present + " read=" + text;
            return present && "avatar".equals(text) && file.delete();
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

    static int relayHeard, targetHeard, offHeard, remoteHeard, hereHeard;

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

    /** A receiver whose manifest names the process "": the app's own, where it hears what Relay hears. */
    public static final class Here extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, android.content.Intent i) { hereHeard++; }
    }

    /** A receiver assigned to another process must not run with this process's Application. */
    public static final class Remote extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, android.content.Intent i) { remoteHeard++; }
    }

    static volatile int stoppedOnMain, stoppedElsewhere;

    /** A service started twice, which stops itself by the first start's id (too old) and then by the second's. */
    public static final class Twice extends android.app.Service {
        private String said;
        @Override public android.os.IBinder onBind(android.content.Intent i) { return null; }
        @Override public int onStartCommand(android.content.Intent i, int flags, int id) {
            if (id == 2) {
                boolean early = stopSelfResult(1);
                said = "early=" + early;
                boolean latest = stopSelfResult(2);
                said += " latest=" + latest;
            }
            return START_NOT_STICKY;
        }
        @Override public void onDestroy() { Log.i("DataTest", "start-ids " + ("early=false latest=true".equals(said) ? "ok" : "FAILED") + " " + said); }
    }

    /* A static of this app's, which its process of another name (Elsewhere's) has a copy of its own of. */
    static int sMarker;

    /** A Parcelable of this app's own, which Elsewhere's process has a class of its own of. */
    public static final class Note implements android.os.Parcelable {
        final String text;
        Note(String text) { this.text = text; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel p, int flags) { p.writeString(text); }
        public static final Creator<Note> CREATOR = new Creator<Note>() {
            @Override public Note createFromParcel(android.os.Parcel p) { return new Note(p.readString()); }
            @Override public Note[] newArray(int n) { return new Note[n]; }
        };
    }

    /**
     * A service in another process (android:process=":elsewhere"), as Play Store's download service is: asked through its
     * binder, it says the name of the process it runs in (its ActivityManager's, for this pid), the app's static sMarker as
     * its process has it, and the kind of loader its class came from.
     */
    public static final class Elsewhere extends android.app.Service {
        @Override public android.os.IBinder onBind(android.content.Intent intent) {
            return new android.os.Binder() {
                @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) {
                    if (code == 3) {
                        // a bundle holding a bundle with a Note of this process's class, in the reply
                        android.os.Bundle details = new android.os.Bundle();
                        details.putParcelable("note", new Note("in a bundle"));
                        android.os.Bundle answer = new android.os.Bundle();
                        answer.putBundle("details", details);
                        reply.writeBundle(answer);
                        return true;
                    }
                    if (code == 2) {
                        // a broadcast with a Note of this process's class, to the app's receiver in its own process
                        sendBroadcast(new android.content.Intent("megler.test.FROM_ELSEWHERE").setPackage(getPackageName()).putExtra("note", new Note("from elsewhere")));
                        return true;
                    }
                    String process = "?";
                    for (android.app.ActivityManager.RunningAppProcessInfo p : ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getRunningAppProcesses())
                        if (p.pid == android.os.Process.myPid()) process = p.processName;
                    reply.writeString("process=" + process + " marker=" + sMarker + " loader=" + Elsewhere.class.getClassLoader().getClass().getSimpleName()
                            + " application=" + (getApplication() != null));
                    return true;
                }
            };
        }
    }

    /** The jobs of the step "job-scheduler": 41 says its extra and finishes later, from a thread; 42 takes its work. */
    public static final class Work extends android.app.job.JobService {
        @Override public boolean onStartJob(final android.app.job.JobParameters p) {
            if (p.getJobId() == 41) {
                Log.i("DataTest", "job 41 said=" + p.getExtras().getString("said") + " network=" + (p.getNetwork() != null));
                new Thread(() -> jobFinished(p, false)).start();
                return true;
            }
            StringBuilder got = new StringBuilder();
            for (android.app.job.JobWorkItem w; (w = p.dequeueWork()) != null; ) {
                got.append(w.getIntent().getAction()).append(',');
                p.completeWork(w);
            }
            Log.i("DataTest", "job " + p.getJobId() + " work=" + got);
            return false;
        }
        @Override public boolean onStopJob(android.app.job.JobParameters p) { return false; }
    }

    /**
     * The job 43 of the step "job-scheduler": a plain Service whose onBind gives its JobServiceEngine's binder, which takes its
     * work on a thread of its own and ends when there is no more, as AndroidX's JobIntentService does on API 26 and later.
     */
    public static final class EngineWork extends android.app.Service {
        private android.app.job.JobServiceEngine engine;
        @Override public void onCreate() {
            engine = new android.app.job.JobServiceEngine(this) {
                @Override public boolean onStartJob(final android.app.job.JobParameters p) {
                    new Thread(() -> {
                        StringBuilder got = new StringBuilder();
                        for (android.app.job.JobWorkItem w; (w = p.dequeueWork()) != null; ) {
                            got.append(w.getIntent().getAction()).append(',');
                            p.completeWork(w);
                        }
                        Log.i("DataTest", "job " + p.getJobId() + " work=" + got + " through its engine");
                    }).start();
                    return true;
                }
                @Override public boolean onStopJob(android.app.job.JobParameters p) { return false; }
            };
        }
        @Override public android.os.IBinder onBind(android.content.Intent i) { return engine.getBinder(); }
    }

    public static final class Again extends android.app.Service {
        private static int made;
        private String said = "";
        @Override public void onCreate() { made++; }
        @Override public android.os.IBinder onBind(android.content.Intent i) { return null; }
        @Override public int onStartCommand(android.content.Intent i, int flags, int id) {
            if (id == 1) {
                startService(new android.content.Intent(this, Again.class));
                said = "first=" + stopSelfResult(1);
            } else if (id == 2) {
                said += " second=" + stopSelfResult(2);
            }
            return START_NOT_STICKY;
        }
        @Override public void onDestroy() {
            said += " made=" + made;
            Log.i("DataTest", "start-asked " + ("first=false second=true made=1".equals(said) ? "ok" : "FAILED") + " " + said);
        }
    }

    /** A binder that says it is an IMessenger and passes every transaction on to another, as GmsCore's task services wrap theirs. */
    static final class Forward extends android.os.Binder {
        private final android.os.IBinder inner;
        Forward(android.os.IBinder inner) { this.inner = inner; attachInterface(null, "android.os.IMessenger"); }
        @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) throws android.os.RemoteException {
            return inner.transact(code, data, reply, flags);
        }
    }

    static volatile int syncSeen;
    static volatile boolean syncSaid;

    /** The provider Main's sync adapter syncs: it holds nothing. */
    public static final class Synced extends android.content.ContentProvider {
        @Override public boolean onCreate() { return true; }
        @Override public android.database.Cursor query(android.net.Uri u, String[] p, String s, String[] a, String o) { return null; }
        @Override public String getType(android.net.Uri u) { return null; }
        @Override public android.net.Uri insert(android.net.Uri u, android.content.ContentValues v) { return null; }
        @Override public int delete(android.net.Uri u, String s, String[] a) { return 0; }
        @Override public int update(android.net.Uri u, android.content.ContentValues v, String s, String[] a) { return 0; }
        /** "content://megler.test.sync/config": the file Main's step provider-client-file wrote, read-only (as Google Play services' Chimera configuration). */
        @Override public android.os.ParcelFileDescriptor openFile(android.net.Uri u, String mode) throws java.io.FileNotFoundException {
            if (!"/config".equals(u.getPath())) throw new java.io.FileNotFoundException(String.valueOf(u));
            return android.os.ParcelFileDescriptor.open(new java.io.File(getContext().getFilesDir(), "provider-config"), android.os.ParcelFileDescriptor.MODE_READ_ONLY);
        }
    }

    /** The service of Main's sync adapter, which hands out the adapter's binder as Android's sync adapters do. */
    public static final class SyncService extends android.app.Service {
        private static SyncAdapter sAdapter;
        @Override public void onCreate() { synchronized (SyncService.class) { if (sAdapter == null) sAdapter = new SyncAdapter(getApplicationContext()); } }
        @Override public android.os.IBinder onBind(android.content.Intent i) { return sAdapter.getSyncAdapterBinder(); }
    }

    /** Says how the sync it was given went: once, the first that is not an initialization. */
    static final class SyncAdapter extends android.content.AbstractThreadedSyncAdapter {
        SyncAdapter(Context c) { super(c, true); }
        @Override public void onPerformSync(android.accounts.Account account, Bundle extras, String authority, android.content.ContentProviderClient provider,
                                            android.content.SyncResult result) {
            result.stats.numInserts++;
            if (syncSaid) return;
            syncSaid = true;
            boolean elsewhere = android.os.Looper.myLooper() != android.os.Looper.getMainLooper();
            int syncable = android.content.ContentResolver.getIsSyncable(account, authority);
            boolean active = android.content.ContentResolver.isSyncActive(account, authority);
            boolean held = elsewhere && provider != null && "megler.test.sync".equals(authority) && "synced".equals(account.name) && syncable == 1 && active
                           && !extras.getBoolean(android.content.ContentResolver.SYNC_EXTRAS_INITIALIZE, false);
            Log.i("DataTest", "sync-performed " + (held ? "ok" : "FAILED") + " thread=" + Thread.currentThread().getName() + " provider=" + (provider != null)
                  + " syncable=" + syncable + " active=" + active + " seen=" + syncSeen + " extras=" + extras.keySet());
        }
    }

    /** A service of the app's that says on which thread it is destroyed. */
    public static final class Stopped extends android.app.Service {
        @Override public android.os.IBinder onBind(android.content.Intent i) { return null; }
        @Override public void onDestroy() {
            if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) stoppedOnMain++;
            else stoppedElsewhere++;
        }
    }

    /**
     * A service stopped from a thread of the app's own is destroyed on the main thread, as on Android (GmsCore's
     * IndexWorkerChimeraService checks that in its onDestroy; Play Store stops it from a background thread). Stopped once its
     * start has gone through, and looked at for up to two seconds after.
     */
    private void serviceStopThread() {
        step("service-stop-thread", () -> {
            final android.content.Intent i = new android.content.Intent(this, Stopped.class);
            startService(i);
            final android.os.Handler h = new android.os.Handler();
            h.post(() -> {
                new Thread(() -> stopService(i), "service-stopper").start();
                h.postDelayed(new Runnable() {
                    int looks;

                    @Override public void run() {
                        if (++looks < 10 && stoppedOnMain + stoppedElsewhere == 0) {
                            h.postDelayed(this, 200);
                            return;
                        }
                        Log.i("DataTest", "service-stop " + (stoppedOnMain == 1 && stoppedElsewhere == 0 ? "ok" : "FAILED") + " main=" + stoppedOnMain + " elsewhere=" + stoppedElsewhere);
                    }
                }, 200);
            });
            return true;
        });
    }

    /**
     * The app's id within its user: its uid modulo the users' range, as UserHandle.getAppId (hidden: by reflection) gives it.
     * The WebView refuses itself to a process whose app id is root's or the system's; the framework answered 0, root's.
     */
    private void userHandle() {
        step("user-handle", () -> {
            int uid = android.os.Process.myUid();
            int appId = (Integer) Class.forName("android.os.UserHandle").getMethod("getAppId", int.class).invoke(null, uid);
            Log.i("DataTest", "user-handle uid=" + uid + " appId=" + appId);
            return uid >= 10000 && appId == uid % 100000;
        });
    }

    /**
     * Cleartext HTTP, as Android's network security policy decides it for this app, from the network security configuration
     * its TLS goes by too (res/xml/network_security_config.xml). Written for Android 9, with a base configuration that says
     * nothing of cleartext, it is refused to example.com and to localhost, whose own configuration says nothing of it either,
     * and so to every host taken together; permitted to a subdomain of sub.megler.test, whose configuration permits it, and to
     * nested.sub.megler.test, whose configuration inside that one takes it from there. The WebView asks this of every http://
     * request; the framework refused it to every app.
     */
    private void cleartext() {
        step("cleartext", () -> {
            android.security.NetworkSecurityPolicy p = android.security.NetworkSecurityPolicy.getInstance();
            return !p.isCleartextTrafficPermitted() && !p.isCleartextTrafficPermitted("example.com") && !p.isCleartextTrafficPermitted("localhost")
                    && p.isCleartextTrafficPermitted("a.sub.megler.test") && p.isCleartextTrafficPermitted("nested.sub.megler.test")
                    && p == android.security.NetworkSecurityPolicy.getInstance();
        });
    }

    /**
     * A compatibility change's state for this app (android.app.compat.CompatChanges, Android 11's system API: by reflection):
     * WebSettings' simplified dark mode, enabled from target SDK 33, is off for an app written for 9; a change the platform
     * does not know is on.
     */
    private void compatChange() {
        step("compat-change", () -> {
            java.lang.reflect.Method m = Class.forName("android.app.compat.CompatChanges").getMethod("isChangeEnabled", long.class);
            return !(Boolean) m.invoke(null, 214741472L) && (Boolean) m.invoke(null, 1L);
        });
    }

    /**
     * A shared library's APK added to the app's resources and files, as Android adds the WebView's (the Android root's
     * /system/app/webview/webview.apk, where tools/dvm-android-root.sh puts it; without it the step has nothing to look at): a
     * cookie of its own, its package given an id from 0x02 that the resources know it by, a resource of it found by name and
     * read, and a file of its assets opened as a descriptor where it is stored, as Chromium maps its ICU data. Adding it
     * again gives the same cookie.
     */
    private void sharedLibrary() {
        step("shared-library", () -> {
            String apk = "/system/app/webview/webview.apk";
            if (!new File(apk).isFile()) {
                Log.i("DataTest", "shared-library: no WebView in the Android root");
                return true;
            }
            android.content.res.AssetManager am = getAssets();
            java.lang.reflect.Method add = am.getClass().getMethod("addAssetPathAsSharedLibrary", String.class);
            int cookie = (Integer) add.invoke(am, apk);
            @SuppressWarnings("unchecked") android.util.SparseArray<String> ids = (android.util.SparseArray<String>) am.getClass().getMethod("getAssignedPackageIdentifiers").invoke(am);
            int pkg = -1;
            for (int i = 0; i < ids.size(); i++) if ("com.android.webview".equals(ids.valueAt(i))) pkg = ids.keyAt(i);
            int icon = getResources().getIdentifier("icon_webview", "drawable", "com.android.webview");
            String label = getResources().getString(getResources().getIdentifier("abc_action_mode_done", "string", "com.android.webview"));
            android.content.res.AssetFileDescriptor icu = am.openNonAssetFd("assets/icudtl.dat");
            long length = icu.getLength(), offset = icu.getStartOffset();
            icu.close();
            Log.i("DataTest", "shared-library cookie=" + cookie + " package=0x" + Integer.toHexString(pkg) + " icon=0x" + Integer.toHexString(icon)
                    + " label=" + label + " icudtl=" + length + "@" + offset);
            return cookie >= 2 && pkg >= 2 && pkg < 0x7f && (icon >>> 24) == pkg && getResources().getDrawable(icon, null) != null
                    && label.length() > 0 && length > 1000000 && offset > 0 && (Integer) add.invoke(am, apk) == cookie;
        });
    }

    /**
     * The cameras there are: none (the phone's are not offered to an app here), said as a device without a camera says it --
     * Camera2's empty list of ids, the old API's count of 0. The WebView counts them through getCameraIdList().length as soon
     * as a page asks for its media devices; the framework's list was null.
     */
    private void cameras() {
        step("cameras", () -> {
            android.hardware.camera2.CameraManager cm = (android.hardware.camera2.CameraManager) getSystemService(CAMERA_SERVICE);
            String[] ids = cm.getCameraIdList();
            Log.i("DataTest", "cameras ids=" + (ids == null ? "null" : String.valueOf(ids.length)) + " old=" + android.hardware.Camera.getNumberOfCameras());
            return ids != null && ids.length == 0 && android.hardware.Camera.getNumberOfCameras() == 0;
        });
    }

    /**
     * Android 13's Safety Center, as Android 13 and later have it on a phone without one: the class is there (Play Store asks
     * for it on its sign-in screen, and ended on a NoClassDefFoundError where it was missing), Context.getSystemService gives
     * the manager, by its class and by its name, and it says Safety Center is off (Google Play services 26 takes the manager
     * with a null check that throws, on Android 14, before it asks isSafetyCenterEnabled).
     */
    private void safetyCenter() {
        step("safety-center", () -> {
            Class<?> c = Class.forName("android.safetycenter.SafetyCenterManager");
            Object m = getSystemService(c);
            Object byName = getSystemService("safety_center");
            Object enabled = m == null ? null : c.getMethod("isSafetyCenterEnabled").invoke(m);
            detail = "class=" + c.getName() + " service=" + m + " by name=" + byName + " enabled=" + enabled;
            return m != null && c.isInstance(m) && byName == m && Boolean.FALSE.equals(enabled);
        });
    }

    /**
     * An intent written in XML (res/xml/intent_probe.xml), read with Intent.parseIntent as AndroidX's preferences read their
     * <intent> child: its action, component, data, category and extras.
     */
    private void parseIntent() {
        step("parse-intent", () -> {
            android.content.res.XmlResourceParser p = getResources().getXml(R.xml.intent_probe);
            int t;
            while ((t = p.next()) != org.xmlpull.v1.XmlPullParser.START_TAG && t != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {}
            android.content.Intent i = android.content.Intent.parseIntent(getResources(), p, p);
            p.close();
            detail = String.valueOf(i) + " categories=" + (i == null ? null : i.getCategories()) + " who=" + (i == null ? null : i.getStringExtra("who"))
                    + " flag=" + (i == null ? null : i.getBooleanExtra("flag", false));
            return i != null && "megler.test.ACTION".equals(i.getAction()) && i.getComponent() != null
                    && "org.microg.gms.auth.login.LoginActivity".equals(i.getComponent().getClassName()) && "com.google.android.gms".equals(i.getComponent().getPackageName())
                    && "https://example.com/path".equals(String.valueOf(i.getData())) && i.hasCategory("android.intent.category.DEFAULT")
                    && "microg".equals(i.getStringExtra("who")) && i.getBooleanExtra("flag", false);
        });
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
            /* Looked at once the main thread is free, and again while the relay has gone out and not arrived, for up to two seconds. */
            final android.os.Handler h = new android.os.Handler();
            h.postDelayed(new Runnable() {
                int looks;

                @Override public void run() {
                    if (++looks < 10 && relayHeard >= 1 && targetHeard == 0) {
                        h.postDelayed(this, 200);
                        return;
                    }
                    unregisterReceiver(r);
                    boolean held = relayHeard == 1 && targetHeard == 1 && offHeard == 0 && remoteHeard == 0 && hereHeard == 1 && registered[0] == 1;
                    Log.i("DataTest", "broadcast-relay " + (held ? "ok" : "FAILED") + " relay=" + relayHeard + " target=" + targetHeard + " off=" + offHeard + " remote=" + remoteHeard + " here=" + hereHeard + " registered=" + registered[0]);
                }
            }, 1500);
            return true;
        });
    }

    /**
     * The platform's MediaMetadata: what its Builder puts in is there, and through a Parcel it comes back by CREATOR -- also from a
     * parcel holding the Bundle alone, which is what MediaMetadataCompat writes when it makes the platform's object.
     */
    private void mediaMetadata() {
        step("media-metadata", () -> {
            android.media.MediaMetadata m = new android.media.MediaMetadata.Builder()
                    .putString(android.media.MediaMetadata.METADATA_KEY_TITLE, "A song")
                    .putString(android.media.MediaMetadata.METADATA_KEY_ARTIST, "Someone")
                    .putLong(android.media.MediaMetadata.METADATA_KEY_DURATION, 215000).build();
            android.os.Parcel p = android.os.Parcel.obtain();
            m.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.media.MediaMetadata back = android.media.MediaMetadata.CREATOR.createFromParcel(p);
            p.recycle();
            android.os.Bundle compat = new android.os.Bundle();
            compat.putCharSequence(android.media.MediaMetadata.METADATA_KEY_TITLE, "From compat");
            android.os.Parcel q = android.os.Parcel.obtain();
            q.writeBundle(compat);
            q.setDataPosition(0);
            android.media.MediaMetadata fromCompat = android.media.MediaMetadata.CREATOR.createFromParcel(q);
            q.recycle();
            detail = "back " + back.getString(android.media.MediaMetadata.METADATA_KEY_TITLE) + "/" + back.getLong(android.media.MediaMetadata.METADATA_KEY_DURATION)
                    + ", compat " + fromCompat.getString(android.media.MediaMetadata.METADATA_KEY_TITLE);
            return m.size() == 3 && "A song".equals(back.getString(android.media.MediaMetadata.METADATA_KEY_TITLE))
                    && back.getLong(android.media.MediaMetadata.METADATA_KEY_DURATION) == 215000 && back.equals(m)
                    && "Someone".contentEquals(back.getDescription().getSubtitle())
                    && "From compat".equals(fromCompat.getString(android.media.MediaMetadata.METADATA_KEY_TITLE));
        });
    }

    /** `frames` of a 440 Hz tone at `rate`, 16-bit mono, as a WAV file. */
    private static void writeWav(java.io.File f, int rate, int frames) throws java.io.IOException {
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(44 + frames * 2).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        b.put("RIFF".getBytes()).putInt(36 + frames * 2).put("WAVE".getBytes()).put("fmt ".getBytes()).putInt(16).putShort((short) 1)
                .putShort((short) 1).putInt(rate).putInt(rate * 2).putShort((short) 2).putShort((short) 16).put("data".getBytes()).putInt(frames * 2);
        for (int i = 0; i < frames; i++) b.putShort((short) (Math.sin(i * 2 * Math.PI * 440 / rate) * 8000));
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) {
            out.write(b.array());
        }
    }

    /**
     * Sound from Java, which megler.Audio plays: a playing AudioTrack of half a second's buffer takes a second of PCM in about
     * half a second, as its blocking write does; a MediaPlayer plays a WAV the step writes and an Ogg Vorbis asset (tone.ogg: 440 Hz, half a second, 22050 Hz
     * mono, made with ffmpeg's libvorbis) through its descriptor, and says when each has played to its end; a SoundPool loads
     * the Ogg and plays it once it is loaded. The later lines are checked by the test.
     */
    private void sound() {
        step("audio-track", () -> {
            int rate = 22050;
            android.media.AudioTrack t = new android.media.AudioTrack(android.media.AudioManager.STREAM_MUSIC, rate, android.media.AudioFormat.CHANNEL_OUT_MONO,
                    android.media.AudioFormat.ENCODING_PCM_16BIT, rate, android.media.AudioTrack.MODE_STREAM);
            short[] pcm = new short[rate];
            for (int i = 0; i < pcm.length; i++) pcm[i] = (short) (Math.sin(i * 2 * Math.PI * 440 / rate) * 8000);
            t.play();
            long t0 = System.nanoTime();
            int written = t.write(pcm, 0, pcm.length);
            long ms = (System.nanoTime() - t0) / 1000000;
            int head = t.getPlaybackHeadPosition();
            t.stop();
            t.release();
            detail = "wrote " + written + " in " + ms + " ms, head at " + head;
            // Its buffer holds half a second (rate bytes of 16-bit mono), as the app asked (6c21a6e0): the first half goes in at
            // once and the write returns once the rest is in, about half a second later, as Android's blocking write does.
            return written == rate && ms >= 400 && head > 0;
        });
        step("media-player", () -> {
            java.io.File wav = new java.io.File(getCacheDir(), "tone.wav");
            writeWav(wav, 22050, 11025);
            android.media.MediaPlayer mp = new android.media.MediaPlayer();
            mp.setDataSource(wav.getPath());
            mp.prepare();
            int duration = mp.getDuration();
            final long t0 = System.nanoTime();
            mp.setOnCompletionListener(p -> {
                long ms = (System.nanoTime() - t0) / 1000000;
                Log.i("DataTest", "media-player-done " + (ms >= 300 ? "ok" : "FAILED") + " after " + ms + " ms");
                p.release();
            });
            mp.start();
            detail = "duration " + duration + " ms";
            return duration >= 450 && duration <= 550;
        });
        step("media-player-ogg", () -> {
            android.content.res.AssetFileDescriptor afd = getAssets().openFd("tone.ogg");
            android.media.MediaPlayer mp = new android.media.MediaPlayer();
            mp.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            mp.prepare();
            afd.close();
            int duration = mp.getDuration();
            final long t0 = System.nanoTime();
            mp.setOnCompletionListener(p -> {
                long ms = (System.nanoTime() - t0) / 1000000;
                Log.i("DataTest", "media-player-ogg-done " + (ms >= 300 ? "ok" : "FAILED") + " after " + ms + " ms");
                p.release();
            });
            mp.start();
            detail = "duration " + duration + " ms";
            return duration >= 450 && duration <= 550;
        });
        step("sound-pool", () -> {
            android.media.SoundPool pool = new android.media.SoundPool(4, android.media.AudioManager.STREAM_MUSIC, 0);
            pool.setOnLoadCompleteListener((p, id, status) -> {
                int stream = status == 0 ? p.play(id, 1f, 1f, 0, 0, 1f) : 0;
                Log.i("DataTest", "sound-pool-played " + (stream > 0 ? "ok" : "FAILED") + " status=" + status + " stream=" + stream);
            });
            int id = pool.load(getAssets().openFd("tone.ogg"), 1);
            detail = "load gave " + id;
            return id > 0;
        });
    }

    /**
     * A compressed stream as an extractor hands it to MediaCodec: tools/media-test-streams.py wrote it (the file's form is
     * said there). The records flagged as codec-specific data are `csd`; the rest are samples in decode order.
     */
    private static final class Stream {
        String mime;
        int width, height, rate, channels;
        final java.util.List<byte[]> csd = new java.util.ArrayList<>(), samples = new java.util.ArrayList<>();
        final java.util.List<Long> times = new java.util.ArrayList<>();

        static Stream read(android.content.res.AssetManager assets, String name) throws java.io.IOException {
            java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
            try (java.io.InputStream in = assets.open(name)) {
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0;) all.write(b, 0, n);
            }
            java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(all.toByteArray()).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            byte[] magic = new byte[8], mime = new byte[32];
            b.get(magic);
            b.get(mime);
            Stream s = new Stream();
            int end = 0;
            while (end < 32 && mime[end] != 0) end++;
            s.mime = new String(mime, 0, end, "US-ASCII");
            s.width = b.getInt();
            s.height = b.getInt();
            s.rate = b.getInt();
            s.channels = b.getInt();
            for (int i = 0, count = b.getInt(); i < count; i++) {
                int flags = b.getInt(), size = b.getInt();
                long time = b.getLong();
                byte[] data = new byte[size];
                b.get(data);
                if ((flags & android.media.MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) s.csd.add(data);
                else {
                    s.samples.add(data);
                    s.times.add(time);
                }
            }
            return s;
        }

        android.media.MediaFormat format(boolean withCsd) {
            android.media.MediaFormat f = mime.startsWith("video/") ? android.media.MediaFormat.createVideoFormat(mime, width, height)
                    : android.media.MediaFormat.createAudioFormat(mime, rate, channels);
            for (int i = 0; withCsd && i < csd.size(); i++) f.setByteBuffer("csd-" + i, java.nio.ByteBuffer.wrap(csd.get(i)));
            return f;
        }
    }

    /** What came out of a decoder: the pictures' times in thirtieths of a second, the sound's bytes and blocks, the formats said, the end. */
    private static final class Decoded {
        final java.util.List<Integer> pictures = new java.util.ArrayList<>();
        long pcm;
        int blocks, formats;
        boolean ended;
        android.media.MediaFormat format;

        boolean inOrder(int first, int count) {
            if (pictures.size() != count) return false;
            for (int i = 0; i < count; i++) if (pictures.get(i) != first + i) return false;
            return true;
        }

        @Override public String toString() {
            return "pictures=" + pictures + " pcm=" + pcm + " blocks=" + blocks + " formats=" + formats + " ended=" + ended + " format=" + format;
        }
    }

    /** Whatever the decoder has ready, taken as an app takes it (pictures shown when `render`); until the end when `untilEnd`. */
    private static void take(android.media.MediaCodec codec, Decoded out, boolean render, boolean untilEnd) {
        android.media.MediaCodec.BufferInfo info = new android.media.MediaCodec.BufferInfo();
        long give = System.nanoTime() + 2000000000L;
        while (!out.ended && System.nanoTime() < give) {
            int i = codec.dequeueOutputBuffer(info, untilEnd ? 10000 : 0);
            if (i == android.media.MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                out.formats++;
                out.format = codec.getOutputFormat();
            } else if (i >= 0) {
                if ((info.flags & android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) out.ended = true;
                if (info.size > 0) {
                    java.nio.ByteBuffer b = codec.getOutputBuffer(i);
                    if (b != null) {
                        out.pcm += b.remaining();
                        out.blocks++;
                    } else {
                        out.pictures.add((int) ((info.presentationTimeUs * 30 + 500000) / 1000000));
                    }
                }
                codec.releaseOutputBuffer(i, render);
            } else if (!untilEnd) {
                return;
            }
        }
    }

    /** The stream's samples from..to (in decode order) through a started decoder, the last with the end of the stream when `end`. */
    private static void feed(android.media.MediaCodec codec, Stream s, int from, int to, boolean end, Decoded out, boolean render) {
        for (int k = from; k < to; k++) {
            int i;
            while ((i = codec.dequeueInputBuffer(10000)) < 0) take(codec, out, render, false);
            java.nio.ByteBuffer b = codec.getInputBuffer(i);
            byte[] data = s.samples.get(k);
            b.put(data);
            codec.queueInputBuffer(i, 0, data.length, s.times.get(k), end && k == to - 1 ? android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
            take(codec, out, render, false);
        }
        if (end) take(codec, out, render, true);
    }

    /** The frame after an ADTS header that says AAC LC at 44100 Hz in two channels. */
    private static byte[] adts(byte[] frame) {
        int len = frame.length + 7;
        byte[] out = new byte[len];
        out[0] = (byte) 0xFF;
        out[1] = (byte) 0xF1;
        out[2] = (byte) (1 << 6 | 4 << 2);
        out[3] = (byte) (2 << 6 | len >> 11);
        out[4] = (byte) (len >> 3);
        out[5] = (byte) ((len & 7) << 5 | 0x1F);
        out[6] = (byte) 0xFC;
        System.arraycopy(frame, 0, out, 7, frame.length);
        return out;
    }

    /**
     * MediaCodec as apps use it, over the shell's decoders (under the test runner, its stand-in, which hands pictures out in
     * decode order: the layer puts them in display order). The streams are the assets avc.mgau (24 pictures with two
     * B-pictures between references, an IDR at 0 and 12), aac.mgau (23 packets) and mp3.mgau (21 frames):
     * - "codec-avc": the parameter sets as csd-0 and csd-1, every picture shown on a Surface over a SurfaceTexture: the
     *   output format first, then all 24 pictures in display order, then the end of the stream; the last one shown is in
     *   the SurfaceTexture, in the grey the stand-in made for its time;
     * - "codec-avc-flush": seven units, a flush, then from the IDR at 12 to the end: pictures 12 to 23 come out, none from
     *   before the flush;
     * - "codec-avc-inband": no csd; the parameter sets in a buffer flagged as codec-specific data first, as some apps send
     *   them;
     * - "codec-avc-async": the callbacks of the asynchronous mode, which ExoPlayer uses, on a thread of their own; the line
     *   "codec-avc-async-done" says what came, and the test runner looks for it;
     * - "codec-aac": csd-0 the AudioSpecificConfig, a block of PCM for each packet, 44100 Hz in two channels;
     * - "codec-aac-adts": no csd, each packet with an ADTS header, as an app that says "is-adts" feeds them;
     * - "codec-mp3": a block for each frame;
     * - "codec-avc-full": units queued with no output taken until dequeueInputBuffer says to try again, which it must say
     *   before all 24 are in, with eight pictures or more waiting (a decoder fills, as Android's do, and players pace
     *   themselves by that); with the waiting ones released it takes input again, and every picture comes out in order.
     */
    private void mediaCodec() {
        step("codec-avc", () -> {
            Stream s = Stream.read(getAssets(), "avc.mgau");
            android.graphics.SurfaceTexture texture = new android.graphics.SurfaceTexture(false);
            texture.setDefaultBufferSize(s.width, s.height);
            android.view.Surface surface = new android.view.Surface(texture);
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType("video/avc");
            codec.configure(s.format(true), surface, null, 0);
            codec.start();
            Decoded out = new Decoded();
            feed(codec, s, 0, s.samples.size(), true, out, true);
            codec.stop();
            codec.release();
            android.graphics.Bitmap shown = (android.graphics.Bitmap) texture.getClass().getMethod("meglerFront").invoke(texture);
            int pixel = shown == null ? 0 : shown.getPixel(s.width / 2, s.height / 2);
            int grey = Math.min(255, (298 * (16 + 8 * 23 - 16) + 128) >> 8);
            surface.release();
            texture.release();
            detail = out + " shown=" + Integer.toHexString(pixel) + " grey=" + grey;
            return out.inOrder(0, 24) && out.ended && out.formats == 1 && out.format.getInteger(android.media.MediaFormat.KEY_WIDTH) == s.width
                    && out.format.getInteger(android.media.MediaFormat.KEY_HEIGHT) == s.height && pixel == (0xFF000000 | grey << 16 | grey << 8 | grey);
        });
        step("codec-avc-flush", () -> {
            Stream s = Stream.read(getAssets(), "avc.mgau");
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType("video/avc");
            codec.configure(s.format(true), null, null, 0);
            codec.start();
            Decoded before = new Decoded(), after = new Decoded();
            feed(codec, s, 0, 7, false, before, false);
            codec.flush();
            int idr = -1;
            for (int k = 0; k < s.times.size(); k++) if ((s.times.get(k) * 30 + 500000) / 1000000 == 12) idr = k;
            feed(codec, s, idr, s.samples.size(), true, after, false);
            codec.release();
            detail = "before: " + before.pictures + " after: " + after + " idr at " + idr;
            return idr > 7 && after.inOrder(12, 12) && after.ended;
        });
        step("codec-avc-inband", () -> {
            Stream s = Stream.read(getAssets(), "avc.mgau");
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType("video/avc");
            codec.configure(s.format(false), null, null, 0);
            codec.start();
            int i = codec.dequeueInputBuffer(10000);
            java.nio.ByteBuffer b = codec.getInputBuffer(i);
            for (byte[] c : s.csd) b.put(c);
            codec.queueInputBuffer(i, 0, b.position(), 0, android.media.MediaCodec.BUFFER_FLAG_CODEC_CONFIG);
            Decoded out = new Decoded();
            feed(codec, s, 0, s.samples.size(), true, out, false);
            codec.release();
            detail = out.toString();
            return out.inOrder(0, 24) && out.ended;
        });
        step("codec-avc-async", () -> {
            final Stream s = Stream.read(getAssets(), "avc.mgau");
            final android.os.HandlerThread thread = new android.os.HandlerThread("codec-callbacks");
            thread.start();
            final android.os.Handler handler = new android.os.Handler(thread.getLooper());
            final android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType("video/avc");
            final Decoded out = new Decoded();
            codec.setCallback(new android.media.MediaCodec.Callback() {
                int next;

                @Override public void onInputBufferAvailable(android.media.MediaCodec c, int index) {
                    if (next >= s.samples.size()) return;
                    byte[] data = s.samples.get(next);
                    c.getInputBuffer(index).put(data);
                    c.queueInputBuffer(index, 0, data.length, s.times.get(next), next == s.samples.size() - 1 ? android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                    next++;
                }

                @Override public void onOutputBufferAvailable(android.media.MediaCodec c, int index, android.media.MediaCodec.BufferInfo info) {
                    if (info.size > 0) out.pictures.add((int) ((info.presentationTimeUs * 30 + 500000) / 1000000));
                    c.releaseOutputBuffer(index, false);
                    if ((info.flags & android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) return;
                    out.ended = true;
                    boolean onThread = android.os.Looper.myLooper() == thread.getLooper();
                    Log.i("DataTest", "codec-avc-async-done " + (out.inOrder(0, 24) && out.formats == 1 && onThread ? "ok" : "FAILED") + " " + out + " on its thread=" + onThread);
                    handler.post(() -> {
                        c.release();
                        thread.quitSafely();
                    });
                }

                @Override public void onError(android.media.MediaCodec c, android.media.MediaCodec.CodecException e) {
                    Log.i("DataTest", "codec-avc-async-done FAILED: " + e);
                }

                @Override public void onOutputFormatChanged(android.media.MediaCodec c, android.media.MediaFormat format) {
                    out.formats++;
                }
            }, handler);
            codec.configure(s.format(true), null, null, 0);
            codec.start();
            return true;
        });
        step("codec-aac", () -> {
            Stream s = Stream.read(getAssets(), "aac.mgau");
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType(s.mime);
            codec.configure(s.format(true), null, null, 0);
            codec.start();
            Decoded out = new Decoded();
            feed(codec, s, 0, s.samples.size(), true, out, false);
            codec.release();
            detail = out + " packets=" + s.samples.size();
            return out.ended && out.blocks == s.samples.size() && out.pcm == s.samples.size() * 1024L * 2 * 2 && out.formats == 1
                    && out.format.getInteger(android.media.MediaFormat.KEY_SAMPLE_RATE) == 44100 && out.format.getInteger(android.media.MediaFormat.KEY_CHANNEL_COUNT) == 2;
        });
        step("codec-aac-adts", () -> {
            Stream s = Stream.read(getAssets(), "aac.mgau");
            for (int k = 0; k < s.samples.size(); k++) s.samples.set(k, adts(s.samples.get(k)));
            android.media.MediaFormat f = s.format(false);
            f.setInteger(android.media.MediaFormat.KEY_IS_ADTS, 1);
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType(s.mime);
            codec.configure(f, null, null, 0);
            codec.start();
            Decoded out = new Decoded();
            feed(codec, s, 0, s.samples.size(), true, out, false);
            codec.release();
            detail = out + " packets=" + s.samples.size();
            return out.ended && out.blocks == s.samples.size() && out.pcm == s.samples.size() * 1024L * 2 * 2;
        });
        step("codec-avc-full", () -> {
            Stream s = Stream.read(getAssets(), "avc.mgau");
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType("video/avc");
            codec.configure(s.format(true), null, null, 0);
            codec.start();
            int queued = 0;
            for (int i; queued < s.samples.size() && (i = codec.dequeueInputBuffer(0)) >= 0; queued++) {
                byte[] data = s.samples.get(queued);
                codec.getInputBuffer(i).put(data);
                codec.queueInputBuffer(i, 0, data.length, s.times.get(queued), 0);
            }
            if (queued == s.samples.size()) {
                codec.release();
                detail = "all " + queued + " units were taken with no picture taken out: the decoder never filled";
                return false;
            }
            boolean full = codec.dequeueInputBuffer(0) < 0;
            Decoded out = new Decoded();
            java.util.List<Integer> waiting = new java.util.ArrayList<>();
            android.media.MediaCodec.BufferInfo info = new android.media.MediaCodec.BufferInfo();
            for (int i; (i = codec.dequeueOutputBuffer(info, 0)) != android.media.MediaCodec.INFO_TRY_AGAIN_LATER;) {
                if (i < 0) continue;
                waiting.add(i);
                out.pictures.add((int) ((info.presentationTimeUs * 30 + 500000) / 1000000));
            }
            for (int i : waiting) codec.releaseOutputBuffer(i, false);
            int back = codec.dequeueInputBuffer(0);
            if (back >= 0) {
                byte[] data = s.samples.get(queued);
                codec.getInputBuffer(back).put(data);
                codec.queueInputBuffer(back, 0, data.length, s.times.get(queued), queued == s.samples.size() - 1 ? android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                take(codec, out, false, false);
                feed(codec, s, queued + 1, s.samples.size(), true, out, false);
            }
            codec.release();
            detail = "queued " + queued + " before it was full, " + waiting.size() + " pictures waiting, input again=" + (back >= 0) + "; " + out;
            return full && waiting.size() >= 8 && back >= 0 && out.inOrder(0, s.samples.size()) && out.ended;
        });
        step("codec-mp3", () -> {
            Stream s = Stream.read(getAssets(), "mp3.mgau");
            android.media.MediaCodec codec = android.media.MediaCodec.createDecoderByType(s.mime);
            codec.configure(s.format(false), null, null, 0);
            codec.start();
            Decoded out = new Decoded();
            feed(codec, s, 0, s.samples.size(), true, out, false);
            codec.release();
            detail = out + " frames=" + s.samples.size();
            return out.ended && out.blocks == s.samples.size() && out.pcm == s.samples.size() * 1152L * 2 * 2;
        });
    }

    /**
     * The result of an activity one app starts in another for one (megler.AppResults), both sides in this app. The C side asks
     * for Answer as another app asks (megler_start_activity); Answer writes the note that com.example.other waits for it, as
     * that app would have, and finishes with a result, which the framework writes in the services' folder before it brings
     * com.example.other to the front. Then this activity asks com.example.other for its activity Main for a result, and puts
     * the result Answer left where com.example.other would have written Main's: once the shell shows this app again (the C side
     * pauses and resumes it), onActivityResult has it -- its code and its extras of plain types.
     */
    public static class Answer extends Activity {
        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            File dir = new File("/data/megler-services/results/" + getPackageName());
            dir.mkdirs();
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(new File(dir, Answer.class.getName() + ".caller"))) {
                out.write("com.example.other".getBytes("UTF-8"));
            } catch (java.io.IOException e) {
                Log.i("DataTest", "app-results: the note cannot be written: " + e);
            }
            setResult(RESULT_OK, new android.content.Intent().putExtra("authAccount", "someone@example.com").putExtra("n", 7).putExtra("skipped", new int[] { 1 }));
            sAnswered = true;
            finish();
        }
    }
    private static boolean sAnswered;

    @Override
    protected void onResume() {
        super.onResume();
        resumedNow = true;
        /* Resumed after the permissions' answer it was paused around (onRequestPermissionsResult). */
        if (resumeAwaited) {
            resumeAwaited = false;
            Log.i("DataTest", "permission-resumed");
        }
        if (sScreens == 1) {
            sScreens = 2;
            startActivity(new android.content.Intent("android.settings.APPLICATION_DETAILS_SETTINGS", android.net.Uri.parse("package:" + getPackageName())));
            return;
        }
        if (sScreens == 2) {
            sScreens = 3;
            startActivity(new android.content.Intent("android.settings.SYNC_SETTINGS").putExtra("account_types", new String[] { "megler.test.none" }));
            return;
        }
        if (!sAnswered) return;
        sAnswered = false;
        File answer = new File("/data/megler-services/results/" + getPackageName() + "/" + Answer.class.getName() + ".json");
        File other = new File("/data/megler-services/results/com.example.other");
        boolean written = answer.isFile();
        startActivityForResult(new android.content.Intent().setClassName("com.example.other", "com.example.other.Main"), 77);
        String note;
        try (java.io.FileInputStream in = new java.io.FileInputStream(new File(other, "com.example.other.Main.caller"))) {
            byte[] b = new byte[64];
            int n = in.read(b);
            note = new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (java.io.IOException e) {
            note = e.toString();
        }
        boolean moved = answer.renameTo(new File(other, "com.example.other.Main.json"));
        Log.i("DataTest", "app-results asked written=" + written + " note=" + note + " moved=" + moved);
    }

    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        Log.i("DataTest", "app-results answered request=" + request + " result=" + result + " authAccount=" + (data == null ? null : data.getStringExtra("authAccount"))
                + " n=" + (data == null ? -1 : data.getIntExtra("n", -1)) + " skipped=" + (data != null && data.hasExtra("skipped")));
        // Then Android's Settings screens that Megler has (megler.SystemScreens), as this activity is in front again: this app's
        // info, then, once that is closed, the accounts.
        if (request == 77) sScreens = 1;
    }
    private static int sScreens;

    /**
     * The other apps the phone has, as a launcher sees them (the test's apps folder has one, com.example.other, "Other",
     * launched by com.example.other.Main): the launcher's intent lists it with its label and icon, it has a launch intent and
     * application info, and starting it asks the shell (megler_system_host.launch_app). And an activity with no icon has
     * the platform's default one, with a size, where it had null (Nova Launcher read the size of it).
     */
    private void phoneApps() {
        step("phone-apps", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            android.content.Intent launcher = new android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_LAUNCHER);
            boolean listed = false;
            for (android.content.pm.ResolveInfo r : pm.queryIntentActivities(launcher, 0))
                if ("com.example.other".equals(r.activityInfo.packageName) && "com.example.other.Main".equals(r.activityInfo.name))
                    listed = "Other".equals(String.valueOf(r.loadLabel(pm))) && r.loadIcon(pm) != null;
            android.content.Intent open = pm.getLaunchIntentForPackage("com.example.other");
            boolean info;
            try { info = pm.getApplicationInfo("com.example.other", 0).enabled; } catch (android.content.pm.PackageManager.NameNotFoundException e) { info = false; }
            boolean launcherApps = false;
            android.content.pm.LauncherApps la = (android.content.pm.LauncherApps) getSystemService(LAUNCHER_APPS_SERVICE);
            for (Object o : la.getActivityList(null, android.os.Process.myUserHandle()))
                if ("com.example.other".equals(((android.content.pm.LauncherActivityInfo) o).getComponentName().getPackageName())) launcherApps = true;
            // An activity with no icon of its own has one all the same, as on Android: the platform's default one at the last.
            android.content.pm.ActivityInfo iconless = new android.content.pm.ActivityInfo();
            iconless.packageName = getPackageName();
            android.graphics.drawable.Drawable fallback = iconless.loadIcon(pm);
            boolean defaultIcon = fallback != null && fallback.getIntrinsicWidth() > 0;
            if (open != null) startActivity(open);
            detail = "listed=" + listed + " open=" + open + " info=" + info + " launcherApps=" + launcherApps + " defaultIcon=" + fallback
                    + (fallback != null ? " " + fallback.getIntrinsicWidth() : "");
            return listed && open != null && "com.example.other.Main".equals(open.getComponent().getClassName()) && info && launcherApps && defaultIcon;
        });
        // Its activities, every one, as the shell described them (the apps folder's com.example.other/activities.json), as a
        // shortcut maker lists them: each with whether it is exported; both launcher entries, the alias's too; the one that
        // makes shortcuts, with its own label and icon (3 x 3, the app's is 2 x 2); not for a link. Then two are started by
        // their classes: Settings, and Second, an alias, which starts what it stands for (the test hears both, launch_app).
        step("phone-apps-activities", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            android.content.pm.PackageInfo p;
            try {
                p = pm.getPackageInfo("com.example.other", android.content.pm.PackageManager.GET_ACTIVITIES);
            } catch (android.content.pm.PackageManager.NameNotFoundException e) { detail = String.valueOf(e); return false; }
            StringBuilder names = new StringBuilder();
            android.content.pm.ActivityInfo settings = null;
            for (android.content.pm.ActivityInfo a : p.activities) {
                names.append(a.name.substring(a.name.lastIndexOf('.') + 1)).append(a.exported ? "+" : "-").append(',');
                if (a.name.endsWith(".Settings")) settings = a;
            }
            int entries = 0;
            android.content.Intent launcher = new android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_LAUNCHER);
            for (android.content.pm.ResolveInfo r : pm.queryIntentActivities(launcher, 0)) if ("com.example.other".equals(r.activityInfo.packageName)) entries++;
            boolean shortcuts = false, link = false;
            for (android.content.pm.ResolveInfo r : pm.queryIntentActivities(new android.content.Intent(android.content.Intent.ACTION_CREATE_SHORTCUT), 0))
                if ("com.example.other.Settings".equals(r.activityInfo.name)) shortcuts = true;
            for (android.content.pm.ResolveInfo r : pm.queryIntentActivities(new android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse("https://example.org/")), 0))
                if ("com.example.other".equals(r.activityInfo.packageName)) link = true;
            android.graphics.drawable.Drawable icon = settings != null ? settings.loadIcon(pm) : null;
            String label = settings != null ? String.valueOf(settings.loadLabel(pm)) : null;
            startActivity(new android.content.Intent().setClassName("com.example.other", "com.example.other.Settings"));
            startActivity(new android.content.Intent().setClassName("com.example.other", "com.example.other.Second"));
            detail = "activities " + names + " entries=" + entries + " shortcuts=" + shortcuts + " link=" + link + " label=" + label
                    + " icon=" + (icon != null ? icon.getIntrinsicWidth() + "x" + icon.getIntrinsicHeight() : "null");
            return "Main+,Settings+,Hidden-,Second+,".equals(names.toString()) && entries == 2 && shortcuts && !link && "Other Settings".equals(label)
                && icon != null && icon.getIntrinsicWidth() == 3;
        });
        // Its signatures are its APK's certificates as the shell described them (the fixture's: 30 03 02 01 07), given when asked
        // for and not otherwise; and it has a uid of its own, which names it.
        step("phone-apps-signatures", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            android.content.pm.PackageInfo plain, sigs, signing;
            try {
                plain = pm.getPackageInfo("com.example.other", 0);
                sigs = pm.getPackageInfo("com.example.other", android.content.pm.PackageManager.GET_SIGNATURES);
                signing = pm.getPackageInfo("com.example.other", android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
            } catch (android.content.pm.PackageManager.NameNotFoundException e) { detail = String.valueOf(e); return false; }
            byte[] want = { 0x30, 0x03, 0x02, 0x01, 0x07 };
            boolean got = sigs.signatures != null && sigs.signatures.length == 1 && java.util.Arrays.equals(sigs.signatures[0].toByteArray(), want);
            boolean signed = signing.signingInfo != null && signing.signingInfo.getApkContentsSigners().length == 1
                    && java.util.Arrays.equals(signing.signingInfo.getApkContentsSigners()[0].toByteArray(), want);
            int uid = plain.applicationInfo.uid;
            detail = "plain=" + java.util.Arrays.toString(plain.signatures) + " sigs=" + got + " signing=" + signed + " uid=" + uid + " named=" + pm.getNameForUid(uid);
            return plain.signatures == null && got && signed && uid != android.os.Process.myUid() && "com.example.other".equals(pm.getNameForUid(uid));
        });
        // Packages compared by their signers: those the phone has match (the app's own, the platform's, another app's); one it does
        // not have is SIGNATURE_UNKNOWN_PACKAGE, as Android answers -- Google Play services took a match of a Pixel's system
        // service with "android" for that service being there, and then did not find it.
        step("check-signatures", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            int own = pm.checkSignatures(getPackageName(), "android"), other = pm.checkSignatures(getPackageName(), "com.example.other");
            int pixel = pm.checkSignatures("com.google.android.pixelsystemservice", "android"), none = pm.checkSignatures(getPackageName(), "megler.test.none");
            int uids = pm.checkSignatures(android.os.Process.myUid(), android.os.Process.myUid());
            detail = "own=" + own + " other=" + other + " pixel=" + pixel + " none=" + none + " uids=" + uids;
            return own == android.content.pm.PackageManager.SIGNATURE_MATCH && other == android.content.pm.PackageManager.SIGNATURE_MATCH
                    && pixel == android.content.pm.PackageManager.SIGNATURE_UNKNOWN_PACKAGE && none == android.content.pm.PackageManager.SIGNATURE_UNKNOWN_PACKAGE
                    && uids == android.content.pm.PackageManager.SIGNATURE_MATCH;
        });
    }

    /**
     * A bitmap's bytes in its config's form, as Android gives them: ALPHA_8 one a pixel (Nova Launcher copies an icon drawn
     * into one into an array of width x height bytes), RGB_565 two in the machine's order, ARGB_8888 four, premultiplied.
     */
    private void bitmapBytes() {
        step("bitmap-bytes", () -> {
            android.graphics.Bitmap alpha = android.graphics.Bitmap.createBitmap(2, 2, android.graphics.Bitmap.Config.ALPHA_8);
            alpha.eraseColor(0x80000000);
            java.nio.ByteBuffer a = java.nio.ByteBuffer.allocate(4);
            alpha.copyPixelsToBuffer(a);
            android.graphics.Bitmap rgb = android.graphics.Bitmap.createBitmap(1, 1, android.graphics.Bitmap.Config.RGB_565);
            rgb.eraseColor(0xFFFF0000);
            java.nio.ByteBuffer r = java.nio.ByteBuffer.allocate(2);
            rgb.copyPixelsToBuffer(r);
            android.graphics.Bitmap full = android.graphics.Bitmap.createBitmap(2, 2, android.graphics.Bitmap.Config.ARGB_8888);
            full.eraseColor(0x80FF0000);
            java.nio.ByteBuffer f = java.nio.ByteBuffer.allocateDirect(16);
            full.copyPixelsToBuffer(f);
            detail = "alpha " + alpha.getByteCount() + "/" + alpha.getRowBytes() + " " + (a.get(0) & 255) + " at " + a.position()
                    + "; 565 " + rgb.getByteCount() + " " + (r.get(0) & 255) + "," + (r.get(1) & 255) + "; 8888 " + full.getByteCount() + " " + (f.get(0) & 255)
                    + "," + (f.get(3) & 255) + " at " + f.position();
            return alpha.getByteCount() == 4 && alpha.getRowBytes() == 2 && (a.get(0) & 255) == 0x80 && (a.get(3) & 255) == 0x80 && a.position() == 4
                    && rgb.getByteCount() == 2 && (r.get(0) & 255) == 0x00 && (r.get(1) & 255) == 0xF8
                    && full.getByteCount() == 16 && Math.abs((f.get(0) & 255) - 0x80) <= 1 && (f.get(3) & 255) == 0x80 && f.position() == 16;
        });
        /*
         * A picture decoded into a bitmap of a pool, as Fresco decodes every still picture (DefaultDecoder): its pool makes a
         * bitmap of 1 x N in RGB_565 for N bytes, hands it over in Options.inBitmap, and throws an IllegalStateException when
         * the bitmap that comes back is another one -- Facebook, React Native's images and Aliucord showed none of theirs. The
         * same bitmap comes back with the picture's size and pixels; reconfigure gives such a bitmap a frame's size, as
         * Fresco's animated pictures do, within what it was made with; a bitmap too small, or recycled, is refused as on
         * Android; an immutable one is passed over for a new bitmap.
         */
        step("bitmap-reuse", () -> {
            android.graphics.Bitmap.Config argb = android.graphics.Bitmap.Config.ARGB_8888, rgb565 = android.graphics.Bitmap.Config.RGB_565;
            android.graphics.Bitmap source = android.graphics.Bitmap.createBitmap(3, 2, argb);
            source.eraseColor(0xFF0000FF);
            source.setPixel(2, 1, 0xFF00FF00);
            java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
            source.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, png);
            byte[] bytes = png.toByteArray();
            android.graphics.Bitmap pool = android.graphics.Bitmap.createBitmap(1, 12, rgb565);  // 24 bytes: 3 x 2 x 4
            android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
            o.inBitmap = pool;
            o.inMutable = true;
            o.inPreferredConfig = argb;
            android.graphics.Bitmap got = android.graphics.BitmapFactory.decodeStream(new java.io.ByteArrayInputStream(bytes), null, o);
            boolean same = got == pool, shaped = got != null && got.getWidth() == 3 && got.getHeight() == 2 && got.getConfig() == argb && got.isMutable();
            int corner = got != null ? got.getPixel(2, 1) : 0, first = got != null ? got.getPixel(0, 0) : 0;
            int allocation = pool.getAllocationByteCount();
            // reconfigure within the 24 bytes, and past them
            pool.reconfigure(2, 3, argb);
            boolean reshaped = pool.getWidth() == 2 && pool.getHeight() == 3 && pool.getByteCount() == 24 && pool.getAllocationByteCount() == 24;
            boolean tooBig = false;
            try { pool.reconfigure(5, 5, argb); } catch (IllegalArgumentException e) { tooBig = true; }
            // a bitmap too small to hold the picture
            android.graphics.BitmapFactory.Options small = new android.graphics.BitmapFactory.Options();
            small.inBitmap = android.graphics.Bitmap.createBitmap(1, 1, argb);
            boolean refused = false;
            try { android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.length, small); } catch (IllegalArgumentException e) { refused = "Problem decoding into existing bitmap".equals(e.getMessage()); }
            // an immutable one, passed over
            android.graphics.BitmapFactory.Options fixed = new android.graphics.BitmapFactory.Options();
            fixed.inBitmap = android.graphics.Bitmap.createBitmap(new int[6], 3, 2, argb);
            android.graphics.Bitmap fresh = android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.length, fixed);
            boolean passedOver = fresh != null && fresh != fixed.inBitmap && fresh.getWidth() == 3;
            detail = "same=" + same + " shaped=" + shaped + " pixels=" + Integer.toHexString(first) + "," + Integer.toHexString(corner) + " allocation=" + allocation
                    + " reshaped=" + reshaped + " tooBig=" + tooBig + " refused=" + refused + " passedOver=" + passedOver;
            return same && shaped && first == 0xFF0000FF && corner == 0xFF00FF00 && allocation == 24 && reshaped && tooBig && refused && passedOver;
        });
    }

    /**
     * A file of the app's made read-only and writable again through java.io.File, as Facebook does with each module it downloads
     * before it loads one ("Downloaded module file must be read only"): setReadOnly, canWrite, an output stream refused, and the
     * file deleted all the same; setWritable gives its writing back. The write bits are kept by the core's chmod (Windows' read-only
     * attribute on a phone), so they are still there when the app starts again.
     */
    private void fileReadOnly() {
        step("file-read-only", () -> {
            File f = new File(getFilesDir(), "module.zip");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) { out.write(new byte[] { 'P', 'K' }); }
            boolean wasWritable = f.canWrite(), made = f.setReadOnly(), readOnly = !f.canWrite() && f.canRead();
            boolean refused = false;
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(f, true)) { out.write(1); } catch (java.io.FileNotFoundException e) { refused = true; }
            boolean writable = f.setWritable(true) && f.canWrite();
            boolean again = f.setReadOnly() && !f.canWrite(), deleted = f.delete() && !f.exists();
            detail = "wasWritable=" + wasWritable + " made=" + made + " readOnly=" + readOnly + " refused=" + refused + " writable=" + writable
                    + " again=" + again + " deleted=" + deleted;
            return wasWritable && made && readOnly && refused && writable && again && deleted;
        });
    }

    /**
     * The enabled state of a component and of an application: of the app's own, the platform's, Google Play services' and the
     * phone's other app's, the default; of a package the phone does not have, IllegalArgumentException, as Android answers.
     * Facebook asks so for Meta's App Manager, and took a default for the App Manager being there: it showed the terms of phones
     * that came with Meta's apps at every start while signed out.
     */
    private void componentState() {
        step("component-state", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            int own = pm.getComponentEnabledSetting(new android.content.ComponentName(this, Main.class));
            int app = pm.getApplicationEnabledSetting(getPackageName()), platform = pm.getApplicationEnabledSetting("android");
            int gms = pm.getComponentEnabledSetting(new android.content.ComponentName("com.google.android.gms", "com.google.android.gms.common.api.GoogleApiActivity"));
            int other = pm.getComponentEnabledSetting(new android.content.ComponentName("com.example.other", "com.example.other.Main"));
            String component = null, application = null;
            try { pm.getComponentEnabledSetting(new android.content.ComponentName("com.facebook.appmanager", "com.facebook.oxygen.appmanager.firstparty.tos.ShouldAcceptTos")); }
            catch (IllegalArgumentException e) { component = e.getMessage(); }
            try { pm.getApplicationEnabledSetting("com.facebook.appmanager"); }
            catch (IllegalArgumentException e) { application = e.getMessage(); }
            detail = "own=" + own + " app=" + app + " platform=" + platform + " gms=" + gms + " other=" + other + " component=" + component
                    + " application=" + application;
            return own == 0 && app == 0 && platform == 0 && gms == 0 && other == 0
                    && "Unknown component: ComponentInfo{com.facebook.appmanager/com.facebook.oxygen.appmanager.firstparty.tos.ShouldAcceptTos}".equals(component)
                    && "Unknown package: com.facebook.appmanager".equals(application);
        });
    }

    /**
     * An object of a class whose interface has 600 interfaces of its own is an instance of each of them, the last ones as
     * well, as Avito's Dagger component is of the 506th of its 564 (Avito ended at its start on a dependency it could not
     * find there).
     */
    private void manyInterfaces() {
        step("many-interfaces", () -> {
            Object o = new ManyInterfaces.Impl();
            boolean first = o instanceof ManyInterfaces.I0, at505 = ManyInterfaces.I505.class.isInstance(o), last = ManyInterfaces.I599.class.isInstance(o);
            boolean cast = ManyInterfaces.I300.class.isAssignableFrom(ManyInterfaces.Impl.class);
            detail = "I0=" + first + " I505=" + at505 + " I599=" + last + " I300 assignable=" + cast;
            return first && at505 && last && cast;
        });
    }

    /**
     * A batch of operations (ContentProviderOperation), as Viber resets its data with one: they were signatures alone, every
     * builder null, and Viber ended at its welcome screen. Applied to a provider of a few rows of its own: two inserts, the
     * second given the first's id, an update and an assertion that find their rows by such references, and a delete. Then to
     * an authority no provider has: its delete removes nothing, and its insert fails as a provider's refused insert does.
     */
    private void batchOperations() {
        step("batch-operations", () -> {
            final java.util.ArrayList<String[]> rows = new java.util.ArrayList<>();                     /* each { _id, name, parent } */
            android.content.ContentProvider p = new android.content.ContentProvider() {
                private boolean matches(String[] r, String s, String[] a) { return s == null || r[0].equals(a[0]); }   /* "_id=?" */
                @Override public boolean onCreate() { return true; }
                @Override public String getType(android.net.Uri u) { return null; }
                @Override public android.database.Cursor query(android.net.Uri u, String[] proj, String s, String[] a, String o) {
                    android.database.MatrixCursor c = new android.database.MatrixCursor(proj);
                    for (String[] r : rows) {
                        if (!matches(r, s, a)) continue;
                        Object[] out = new Object[proj.length];
                        for (int i = 0; i < proj.length; i++) out[i] = "name".equals(proj[i]) ? r[1] : "parent".equals(proj[i]) ? r[2] : r[0];
                        c.addRow(out);
                    }
                    return c;
                }
                @Override public android.net.Uri insert(android.net.Uri u, android.content.ContentValues v) {
                    rows.add(new String[] { String.valueOf(rows.size() + 1), v.getAsString("name"), v.getAsString("parent") });
                    return android.content.ContentUris.withAppendedId(u, rows.size());
                }
                @Override public int update(android.net.Uri u, android.content.ContentValues v, String s, String[] a) {
                    int n = 0;
                    for (String[] r : rows) if (matches(r, s, a)) { r[1] = v.getAsString("name"); n++; }
                    return n;
                }
                @Override public int delete(android.net.Uri u, String s, String[] a) {
                    int n = 0;
                    for (java.util.Iterator<String[]> i = rows.iterator(); i.hasNext(); ) if (matches(i.next(), s, a)) { i.remove(); n++; }
                    return n;
                }
            };
            android.net.Uri table = android.net.Uri.parse("content://megler.test.rows/rows");
            java.util.ArrayList<android.content.ContentProviderOperation> ops = new java.util.ArrayList<>();
            ops.add(android.content.ContentProviderOperation.newInsert(table).withValue("name", "first").build());
            ops.add(android.content.ContentProviderOperation.newInsert(table).withValue("name", "second").withValueBackReference("parent", 0).build());
            ops.add(android.content.ContentProviderOperation.newUpdate(table).withValue("name", "changed").withSelection("_id=?", null)
                    .withSelectionBackReference(0, 1).withExpectedCount(1).build());
            ops.add(android.content.ContentProviderOperation.newAssertQuery(table).withValue("name", "first").withSelection("_id=?", null)
                    .withSelectionBackReference(0, 0).withExpectedCount(1).build());
            ops.add(android.content.ContentProviderOperation.newDelete(table).withSelection("_id=?", new String[] { "1" }).build());
            android.content.ContentProviderResult[] r = p.applyBatch(ops);
            boolean applied = r.length == 5 && "content://megler.test.rows/rows/1".equals(String.valueOf(r[0].uri)) && "content://megler.test.rows/rows/2".equals(String.valueOf(r[1].uri))
                    && r[2].count == 1 && r[3].count == 1 && r[4].count == 1 && rows.size() == 1 && "changed".equals(rows.get(0)[1]) && "1".equals(rows.get(0)[2]);
            // An authority nobody provides here: as a provider with no rows answers.
            java.util.ArrayList<android.content.ContentProviderOperation> none = new java.util.ArrayList<>();
            none.add(android.content.ContentProviderOperation.newDelete(android.net.Uri.parse("content://megler.test.none/rows")).build());
            android.content.ContentProviderResult[] deleted = getContentResolver().applyBatch("megler.test.none", none);
            none.add(android.content.ContentProviderOperation.newInsert(android.net.Uri.parse("content://megler.test.none/rows")).withValue("name", "x").build());
            String refused = null;
            try { getContentResolver().applyBatch("megler.test.none", none); } catch (android.content.OperationApplicationException e) { refused = e.getMessage(); }
            detail = java.util.Arrays.toString(r) + " rows=" + rows.size() + " none=" + java.util.Arrays.toString(deleted) + " refused=" + refused;
            return applied && deleted.length == 1 && deleted[0].count == 0 && refused != null;
        });
    }

    /**
     * A proxy of an interface of 600 methods, as imo's settings are a proxy of one of 1968 (imo ended at its start when there
     * could be no more than 512): made, and its first and last methods reach the handler.
     */
    private void proxyManyMethods() {
        step("proxy-many-methods", () -> {
            ManyMethods p = (ManyMethods) java.lang.reflect.Proxy.newProxyInstance(getClassLoader(), new Class<?>[] { ManyMethods.class },
                    (proxy, m, args) -> Integer.parseInt(m.getName().substring(1)));
            int first = p.m0(), last = p.m599();
            detail = "m0=" + first + " m599=" + last;
            return first == 0 && last == 599;
        });
    }

    /**
     * A URI built with a scheme and a path that is not absolute, as MAX builds its routes: Android makes the path absolute,
     * and the URI is hierarchical, with that path.
     */
    private void uriBuilder() {
        step("uri-builder", () -> {
            android.net.Uri u = new android.net.Uri.Builder().scheme("max").encodedAuthority(null).encodedPath(":chat-list").build();
            android.net.Uri bare = new android.net.Uri.Builder().encodedPath("a/b").build();
            detail = u + " path=" + u.getPath() + " hierarchical=" + u.isHierarchical() + "; " + bare + " path=" + bare.getPath();
            return "max:/:chat-list".equals(u.toString()) && "/:chat-list".equals(u.getPath()) && u.isHierarchical() && "a/b".equals(bare.getPath());
        });
        step("html-tag-handler", () -> {
            // Html.fromHtml as Android's: the spans of the elements it knows, a paragraph's line breaks after it, elements left
            // open ended with the text, character references read; and a TagHandler is given html and body before the rest,
            // with the reader, whose ContentHandler it takes to read an element's attributes (VTB's way: its handleTag called
            // getContentHandler() on the reader, which was null here).
            final java.util.ArrayList<String> tags = new java.util.ArrayList<>();
            final String[] attr = { null };
            final class Attrs implements android.text.Html.TagHandler, org.xml.sax.ContentHandler {
                org.xml.sax.ContentHandler wrapped;
                public void handleTag(boolean opening, String tag, android.text.Editable out, org.xml.sax.XMLReader reader) {
                    tags.add((opening ? "+" : "-") + tag);
                    if (wrapped == null) {
                        wrapped = reader.getContentHandler();
                        reader.setContentHandler(this);
                    }
                }
                public void startElement(String uri, String local, String q, org.xml.sax.Attributes a) throws org.xml.sax.SAXException {
                    if ("mark".equals(local)) attr[0] = a.getValue("", "data-x");
                    wrapped.startElement(uri, local, q, a);
                }
                public void endElement(String uri, String local, String q) throws org.xml.sax.SAXException { wrapped.endElement(uri, local, q); }
                public void characters(char[] ch, int s, int l) throws org.xml.sax.SAXException { wrapped.characters(ch, s, l); }
                public void setDocumentLocator(org.xml.sax.Locator l) {}
                public void startDocument() {}
                public void endDocument() {}
                public void startPrefixMapping(String p, String u) {}
                public void endPrefixMapping(String p) {}
                public void ignorableWhitespace(char[] ch, int s, int l) {}
                public void processingInstruction(String t, String d) {}
                public void skippedEntity(String n) {}
            }
            android.text.Spanned custom = android.text.Html.fromHtml("<mark data-x=\"7\">a</mark>b", null, new Attrs());
            android.text.Spanned styled = android.text.Html.fromHtml("<b>bold</b> and <i>it</i><br>next", 0);
            android.text.style.StyleSpan[] styles = styled.getSpans(0, styled.length(), android.text.style.StyleSpan.class);
            boolean stylesOk = styles.length == 2 && styles[0].getStyle() == android.graphics.Typeface.BOLD && styled.getSpanStart(styles[0]) == 0
                    && styled.getSpanEnd(styles[0]) == 4 && styles[1].getStyle() == android.graphics.Typeface.ITALIC && styled.getSpanStart(styles[1]) == 9
                    && styled.getSpanEnd(styles[1]) == 11;
            String legacy = android.text.Html.fromHtml("<p>one</p><p>two</p>", android.text.Html.FROM_HTML_MODE_LEGACY).toString();
            String compact = android.text.Html.fromHtml("<p>one</p><p>two</p>", android.text.Html.FROM_HTML_MODE_COMPACT).toString();
            android.text.Spanned list = android.text.Html.fromHtml("<ul><li>a</li><li>b</li></ul>", 0);
            int bullets = list.getSpans(0, list.length(), android.text.style.BulletSpan.class).length;
            String refs = android.text.Html.fromHtml("&lt;b&gt; &amp; &copy;&#169;&#xA9;&nbsp;x").toString();
            android.text.Spanned link = android.text.Html.fromHtml("<a href=\"https://example.com/?a=1&amp;b=2\">l</a>");
            android.text.style.URLSpan[] urls = link.getSpans(0, link.length(), android.text.style.URLSpan.class);
            android.text.Spanned open = android.text.Html.fromHtml("<b>x<i>y");
            int openStyles = open.getSpans(0, open.length(), android.text.style.StyleSpan.class).length;
            detail = "tags=" + tags + " attr=" + attr[0] + " custom=" + custom + " styled=" + styled.toString().replace("\n", "\\n") + " styles=" + stylesOk
                    + " legacy=" + legacy.replace("\n", "\\n") + " compact=" + compact.replace("\n", "\\n") + " list=" + list.toString().replace("\n", "\\n")
                    + " bullets=" + bullets + " refs=" + refs + " url=" + (urls.length == 1 ? urls[0].getURL() : "none") + " open=" + open + "/" + openStyles;
            return "[+html, +body, +mark, -mark, -body, -html]".equals(tags.toString()) && "7".equals(attr[0]) && "ab".equals(custom.toString())
                    && "bold and it\nnext".equals(styled.toString()) && stylesOk && "one\n\ntwo\n\n".equals(legacy) && "one\ntwo\n".equals(compact)
                    && "a\n\nb\n\n".equals(list.toString()) && bullets == 2 && "<b> & \u00A9\u00A9\u00A9\u00A0x".equals(refs)
                    && urls.length == 1 && "https://example.com/?a=1&b=2".equals(urls[0].getURL()) && "xy".equals(open.toString()) && openStyles == 2;
        });
        step("stats-manager", () -> {
            // statsd's manager, which Google Play services hands its anomaly configurations (a system API: by reflection): there
            // by its name and by its class, and each call says statsd is not running, with StatsUnavailableException, as Android
            // says it then.
            Class<?> c = Class.forName("android.app.StatsManager");
            Object byName = getSystemService("stats"), byClass = getSystemService(c);
            String thrown = null;
            try {
                c.getMethod("addConfig", long.class, byte[].class).invoke(byName, 1L, new byte[0]);
            } catch (java.lang.reflect.InvocationTargetException e) {
                thrown = e.getCause().getClass().getName();
            }
            detail = "byName=" + byName + " byClass=" + byClass + " thrown=" + thrown;
            return c.isInstance(byName) && byName == byClass && "android.app.StatsManager$StatsUnavailableException".equals(thrown);
        });
        step("credential-manager", () -> {
            // Android 14's Credential Manager with no credential provider, as a sign-in screen asks it (an API 34 class: by
            // reflection): getCredential answers on the executor given that there is no credential -- the type AndroidX turns into
            // NoCredentialException --, createCredential that there is nothing to create one with, clearCredentialState that it
            // is cleared. Avito's sign-in waited for ever on a getCredential that never answered.
            Class<?> cm = Class.forName("android.credentials.CredentialManager"), receiver = Class.forName("android.os.OutcomeReceiver");
            Object manager = getSystemService(cm);
            final java.util.List<String> heard = java.util.Collections.synchronizedList(new java.util.ArrayList<String>());
            final java.util.concurrent.CountDownLatch answered = new java.util.concurrent.CountDownLatch(3);
            java.lang.reflect.InvocationHandler h = (proxy, m, a) -> {
                if ("onError".equals(m.getName())) {
                    heard.add("error " + a[0].getClass().getMethod("getType").invoke(a[0]));
                    answered.countDown();
                } else if ("onResult".equals(m.getName())) {
                    heard.add("result " + a[0]);
                    answered.countDown();
                }
                return null;
            };
            Object callback = java.lang.reflect.Proxy.newProxyInstance(receiver.getClassLoader(), new Class<?>[] { receiver }, h);
            java.util.concurrent.Executor direct = Runnable::run;
            Class<?> getRequest = Class.forName("android.credentials.GetCredentialRequest"), createRequest = Class.forName("android.credentials.CreateCredentialRequest"),
                    clearRequest = Class.forName("android.credentials.ClearCredentialStateRequest");
            cm.getMethod("getCredential", android.content.Context.class, getRequest, android.os.CancellationSignal.class, java.util.concurrent.Executor.class, receiver)
                    .invoke(manager, this, null, new android.os.CancellationSignal(), direct, callback);
            cm.getMethod("createCredential", android.content.Context.class, createRequest, android.os.CancellationSignal.class, java.util.concurrent.Executor.class, receiver)
                    .invoke(manager, this, null, null, direct, callback);
            cm.getMethod("clearCredentialState", clearRequest, android.os.CancellationSignal.class, java.util.concurrent.Executor.class, receiver)
                    .invoke(manager, null, null, direct, callback);
            boolean all = answered.await(5, java.util.concurrent.TimeUnit.SECONDS);
            detail = "manager=" + (manager != null) + " heard=" + heard;
            return manager != null && all && heard.equals(java.util.Arrays.asList("error android.credentials.GetCredentialException.TYPE_NO_CREDENTIAL",
                    "error android.credentials.CreateCredentialException.TYPE_NO_CREATE_OPTIONS", "result null"));
        });
    }

    /**
     * What Launcher3 leans on while it loads its apps, which Nova Launcher stopped on. The main thread's idle handlers run
     * when nothing is due: one that answers false runs once, one that answers true again at each idle spell (Launcher3
     * waits that way before each step); the harness waits for "idle-handler-done". A widget's info names its user
     * (getProfile, which Launcher3's ComponentKey refuses as null). And the launcher may host shortcuts, as the phone's
     * home screen does.
     */
    private void launcherLoading() {
        step("idle-handler", () -> {
            final int[] once = { 0 }, kept = { 0 };
            final android.os.MessageQueue q = android.os.Looper.myQueue();
            final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
            q.addIdleHandler(() -> { once[0]++; return false; });
            // Idle handlers run once for each message the queue gives out: this one posts one each time, so that the main
            // thread is idle again after it, and says what it saw at its third time.
            q.addIdleHandler(() -> {
                if (++kept[0] < 3) { h.post(() -> {}); return true; }
                Log.i("DataTest", "idle-handler-done " + (once[0] == 1 ? "ok" : "FAILED") + " once=" + once[0] + " kept=" + kept[0]);
                return false;
            });
            android.appwidget.AppWidgetProviderInfo widget = new android.appwidget.AppWidgetProviderInfo();
            android.content.pm.LauncherApps la = (android.content.pm.LauncherApps) getSystemService(LAUNCHER_APPS_SERVICE);
            detail = "profile=" + widget.getProfile() + " shortcutHost=" + la.hasShortcutHostPermission();
            return android.os.Process.myUserHandle().equals(widget.getProfile()) && la.hasShortcutHostPermission();
        });
    }

    /**
     * An AdaptiveIconDrawable as a launcher makes one to learn the icon's shape (Launcher3's GraphicsUtils.getShapePath):
     * its mask is there, in its bounds; its layers are half as large again as the icon, centred; drawn, its middle is the
     * background's colour.
     */
    private void adaptiveIcon() {
        step("adaptive-icon", () -> {
            android.graphics.drawable.AdaptiveIconDrawable d = new android.graphics.drawable.AdaptiveIconDrawable(
                    new android.graphics.drawable.ColorDrawable(0xFF0000FF), new android.graphics.drawable.ColorDrawable(0));
            d.setBounds(0, 0, 100, 100);
            android.graphics.Path mask = new android.graphics.Path(d.getIconMask());
            android.graphics.RectF box = new android.graphics.RectF();
            mask.computeBounds(box, true);
            android.graphics.Rect layer = d.getBackground().getBounds();
            android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(100, 100, android.graphics.Bitmap.Config.ARGB_8888);
            d.draw(new android.graphics.Canvas(b));
            int middle = b.getPixel(50, 50);
            detail = "mask " + box + " layer " + layer + " middle " + Integer.toHexString(middle) + " inset " + android.graphics.drawable.AdaptiveIconDrawable.getExtraInsetFraction();
            return !mask.isEmpty() && box.width() > 90 && box.width() <= 101 && layer.left == -25 && layer.right == 125 && middle == 0xFF0000FF
                    && android.graphics.drawable.AdaptiveIconDrawable.getExtraInsetFraction() == 0.25f;
        });
    }

    /**
     * An <inset> whose insets are fractions of its bounds (res/drawable/inset_fraction.xml: a quarter on every side but the
     * left, which is 8 pixels, round a shape of 80 x 40), inflated as Android 8 and later inflate it: in bounds of 200 x 100 the
     * shape is at 8, 25 to 150, 75; the intrinsic size is the shape's made the part of the whole that the fractions leave
     * (80 / 0.75 + 8 = 114, 40 / 0.5 = 80); the padding is the insets. And the inset of 0.01% WhatsApp's adaptive icon has round
     * its foreground (res/drawable/inset_tiny.xml), which ended the app where the inflater took every inset for a dimension; and
     * InsetDrawable(Drawable, float), a quarter on every side of 40 x 40.
     */
    private void insetFraction() {
        step("inset-fraction", () -> {
            android.graphics.drawable.Drawable d = getResources().getDrawable(R.drawable.inset_fraction, null);
            android.graphics.drawable.Drawable tiny = getResources().getDrawable(R.drawable.inset_tiny, null);
            if (!(d instanceof android.graphics.drawable.InsetDrawable) || !(tiny instanceof android.graphics.drawable.InsetDrawable)) {
                detail = "drawables " + d + " " + tiny;
                return false;
            }
            d.setBounds(0, 0, 200, 100);
            android.graphics.Rect inner = new android.graphics.Rect(((android.graphics.drawable.InsetDrawable) d).getDrawable().getBounds());
            android.graphics.Rect pad = new android.graphics.Rect();
            boolean padded = d.getPadding(pad);
            android.graphics.drawable.InsetDrawable made = new android.graphics.drawable.InsetDrawable(new android.graphics.drawable.ColorDrawable(0xFF0000FF), 0.25f);
            made.setBounds(0, 0, 40, 40);
            android.graphics.Rect madeInner = made.getDrawable().getBounds();
            boolean nested = ((android.graphics.drawable.InsetDrawable) tiny).getDrawable() instanceof android.graphics.drawable.InsetDrawable;
            detail = "inner " + inner + " intrinsic " + d.getIntrinsicWidth() + "x" + d.getIntrinsicHeight() + " padding " + pad + " made " + madeInner + " nested " + nested;
            return inner.equals(new android.graphics.Rect(8, 25, 150, 75)) && d.getIntrinsicWidth() == 114 && d.getIntrinsicHeight() == 80
                    && padded && pad.equals(new android.graphics.Rect(8, 25, 50, 25)) && madeInner.equals(new android.graphics.Rect(10, 10, 30, 30)) && nested;
        });
    }

    /**
     * A WorkSource through a Parcel as Google Play services' client library writes a location request's: its writeToParcel
     * inside a field's header whose size is patched in after (SafeParcelWriter), read back with the CREATOR, as Google Play
     * services' location service reads it. One with two uids and a name each and a work chain comes back equal, kept in the
     * uids' order as Android keeps it; an empty one comes back empty, not null -- the null that ended every request of
     * FusedLocationProviderClient in Google Play services' ImmutableWorkSource. (Most of WorkSource is system API, not in the
     * SDK the app is built against: by reflection.)
     */
    private void workSource() {
        step("work-source", () -> {
            Class<android.os.WorkSource> c = android.os.WorkSource.class;
            android.os.WorkSource ws = new android.os.WorkSource(), empty = new android.os.WorkSource();
            java.lang.reflect.Method add = c.getMethod("add", int.class, String.class);
            add.invoke(ws, 10123, "megler.test.b");
            add.invoke(ws, 10045, "megler.test.a");
            Object chain = c.getMethod("createWorkChain").invoke(ws);
            chain.getClass().getMethod("addNode", int.class, String.class).invoke(chain, 10045, "location");
            android.os.Parcel p = android.os.Parcel.obtain();
            p.writeInt(0xFFFF0001);
            int sizeAt = p.dataPosition();
            p.writeInt(0);
            int start = p.dataPosition();
            ws.writeToParcel(p, 0);
            int end = p.dataPosition();
            p.setDataPosition(sizeAt);
            p.writeInt(end - start);
            p.setDataPosition(end);
            empty.writeToParcel(p, 0);
            p.setDataPosition(0);
            p.readInt();
            int size = p.readInt();
            android.os.WorkSource back = android.os.WorkSource.CREATOR.createFromParcel(p);
            android.os.WorkSource emptyBack = android.os.WorkSource.CREATOR.createFromParcel(p);
            p.recycle();
            int n = (Integer) c.getMethod("size").invoke(back);
            int firstUid = (Integer) c.getMethod("getUid", int.class).invoke(back, 0);
            String firstName = (String) c.getMethod("getPackageName", int.class).invoke(back, 0);
            java.util.List<?> chains = (java.util.List<?>) c.getMethod("getWorkChains").invoke(back);
            boolean emptyIs = emptyBack != null && (Boolean) c.getMethod("isEmpty").invoke(emptyBack);
            detail = "size=" + size + " back=" + back + " empty=" + emptyBack;
            return size > 0 && back != null && back.equals(ws) && !back.equals(empty) && n == 2 && firstUid == 10045 && "megler.test.a".equals(firstName)
                    && chains != null && chains.size() == 1 && emptyIs && emptyBack.equals(empty);
        });
    }

    /**
     * A Parcelable kept as its marshalled bytes, as Facebook keeps an activity's (OpaqueParcelable: writeToParcel into a parcel
     * of its own, marshall), and restored from them in another parcel (unmarshall, setDataPosition(0), its creator): what it
     * wrote as objects -- a list, a bundle -- comes back, the parcel's data the size it was written; bytes of a parcel without
     * objects are its data alone. Messenger read null for the list its photo editor's layers were, and ended.
     */
    private void parcelMarshall() {
        step("parcel-marshall", () -> {
            android.os.Parcel w = android.os.Parcel.obtain();
            java.util.ArrayList<String> layers = new java.util.ArrayList<>(java.util.Arrays.asList("text", "sticker"));
            android.os.Bundle extra = new android.os.Bundle();
            extra.putString("tool", "pen");
            w.writeInt(7);
            w.writeList(layers);
            w.writeBundle(extra);
            w.writeString("end");
            int written = w.dataSize();
            byte[] bytes = w.marshall();
            w.recycle();
            android.os.Parcel r = android.os.Parcel.obtain();
            r.unmarshall(bytes, 0, bytes.length);
            r.setDataPosition(0);
            int size = r.dataSize(), seven = r.readInt();
            java.util.ArrayList<?> back = r.readArrayList(getClassLoader());
            android.os.Bundle b = r.readBundle(getClassLoader());
            String end = r.readString();
            r.recycle();
            android.os.Parcel plain = android.os.Parcel.obtain();
            plain.writeInt(1);
            plain.writeString("only data");
            int plainSize = plain.dataSize(), plainBytes = plain.marshall().length;
            plain.recycle();
            detail = "size=" + size + " of " + written + " list=" + back + " bundle=" + (b == null ? null : b.getString("tool")) + " end=" + end
                    + " plain=" + plainBytes + " of " + plainSize;
            return size == written && seven == 7 && layers.equals(back) && b != null && "pen".equals(b.getString("tool")) && "end".equals(end) && plainBytes == plainSize;
        });
    }

    /**
     * Android 13's PackageManager calls with a flags object answer as the calls with an int do: the app's own service, provider,
     * receivers and uid found, its signatures given when asked for. Some were stubs that found nothing (Facebook Lite's
     * queryIntentServices and resolveContentProvider, Messenger's resolveContentProvider), some passed no flags on. (The
     * flags classes are not in the SDK the app is built against: by reflection.)
     */
    private void packageFlags() {
        step("package-flags", () -> {
            android.content.pm.PackageManager pm = getPackageManager();
            Class<?> pmc = android.content.pm.PackageManager.class;
            Class<?> rif = Class.forName("android.content.pm.PackageManager$ResolveInfoFlags"), cif = Class.forName("android.content.pm.PackageManager$ComponentInfoFlags");
            Class<?> pif = Class.forName("android.content.pm.PackageManager$PackageInfoFlags"), aif = Class.forName("android.content.pm.PackageManager$ApplicationInfoFlags");
            Object r0 = rif.getMethod("of", long.class).invoke(null, 0L), c0 = cif.getMethod("of", long.class).invoke(null, 0L);
            Object p0 = pif.getMethod("of", long.class).invoke(null, 0L);
            Object pSigned = pif.getMethod("of", long.class).invoke(null, (long) android.content.pm.PackageManager.GET_SIGNATURES);
            Object aMeta = aif.getMethod("of", long.class).invoke(null, (long) android.content.pm.PackageManager.GET_META_DATA);
            String own = getPackageName();
            android.content.Intent service = new android.content.Intent().setClassName(own, Main.class.getName() + "$Twice");
            android.content.Intent relay = new android.content.Intent("megler.test.RELAY");
            java.util.List<?> services = (java.util.List<?>) pmc.getMethod("queryIntentServices", android.content.Intent.class, rif).invoke(pm, service, r0);
            Object resolved = pmc.getMethod("resolveService", android.content.Intent.class, rif).invoke(pm, service, r0);
            Object provider = pmc.getMethod("resolveContentProvider", String.class, cif).invoke(pm, "megler.test.sync", c0);
            java.util.List<?> receivers = (java.util.List<?>) pmc.getMethod("queryBroadcastReceivers", android.content.Intent.class, rif).invoke(pm, relay, r0);
            Object receiver = pmc.getMethod("getReceiverInfo", android.content.ComponentName.class, cif).invoke(pm, new android.content.ComponentName(own, Main.class.getName() + "$Relay"), c0);
            int uid = (Integer) pmc.getMethod("getPackageUid", String.class, pif).invoke(pm, own, p0);
            android.content.pm.PackageInfo signed = null;
            for (Object o : (java.util.List<?>) pmc.getMethod("getInstalledPackages", pif).invoke(pm, pSigned))
                if (own.equals(((android.content.pm.PackageInfo) o).packageName)) signed = (android.content.pm.PackageInfo) o;
            java.util.List<?> apps = (java.util.List<?>) pmc.getMethod("getInstalledApplications", aif).invoke(pm, aMeta);
            int intReceivers = pm.queryBroadcastReceivers(relay, 0).size();
            boolean intSigned = pm.getPackageInfo(own, android.content.pm.PackageManager.GET_SIGNATURES).signatures != null;
            detail = "services=" + services.size() + " resolved=" + (resolved != null) + " provider=" + (provider != null) + " receivers=" + receivers.size() + " of " + intReceivers
                    + " receiver=" + (receiver != null) + " uid=" + uid + " signed=" + (signed == null ? "none" : signed.signatures != null) + " apps=" + apps.size();
            return services.size() == 1 && resolved != null && provider != null && intReceivers > 0 && receivers.size() == intReceivers && receiver != null
                    && uid == android.os.Process.myUid() && signed != null && (signed.signatures != null) == intSigned && !apps.isEmpty();
        });
    }

    /**
     * Bitmaps through a Parcel as Google Play services' maps client writes a marker's picture (BitmapDescriptorFactory.fromBitmap:
     * an int, then the bitmap's writeToParcel) and its maps module reads it (an int, then Bitmap.CREATOR): each comes out a
     * bitmap of its own, of the same size, pixels, density and mutability, the parcel read to its end -- the module checks
     * that -- and whole after the writer has recycled its own.
     */
    private void bitmapParcel() {
        step("bitmap-parcel", () -> {
            android.graphics.Bitmap mutable = android.graphics.Bitmap.createBitmap(3, 2, android.graphics.Bitmap.Config.ARGB_8888);
            mutable.setPixel(0, 0, 0xFFFF0000);
            mutable.setPixel(2, 1, 0xFF00FF00);
            mutable.setDensity(320);
            android.graphics.Bitmap fixed = android.graphics.Bitmap.createBitmap(new int[] { 0xFF0000FF, 0xFFFFFFFF }, 2, 1, android.graphics.Bitmap.Config.ARGB_8888);
            android.os.Parcel p = android.os.Parcel.obtain();
            for (android.graphics.Bitmap b : new android.graphics.Bitmap[] { mutable, fixed }) {
                p.writeInt(1);
                b.writeToParcel(p, 0);
            }
            p.setDataPosition(0);
            android.graphics.Bitmap[] back = android.graphics.Bitmap.CREATOR.newArray(2);
            for (int i = 0; i < back.length; i++) back[i] = p.readInt() != 0 ? android.graphics.Bitmap.CREATOR.createFromParcel(p) : null;
            int left = p.dataAvail();
            p.recycle();
            mutable.recycle();
            fixed.recycle();
            android.graphics.Bitmap a = back[0], b = back[1];
            detail = "back=" + java.util.Arrays.toString(back) + " left=" + left
                    + (a == null || a.isRecycled() ? "" : " first " + a.getWidth() + "x" + a.getHeight() + " density=" + a.getDensity() + " mutable=" + a.isMutable());
            return a != null && b != null && a != mutable && b != fixed && !a.isRecycled() && !b.isRecycled() && left == 0
                    && a.getWidth() == 3 && a.getHeight() == 2 && a.getDensity() == 320 && a.isMutable() && a.getConfig() == android.graphics.Bitmap.Config.ARGB_8888
                    && a.getPixel(0, 0) == 0xFFFF0000 && a.getPixel(2, 1) == 0xFF00FF00 && a.getPixel(1, 0) == 0
                    && b.getWidth() == 2 && b.getHeight() == 1 && !b.isMutable() && b.getPixel(0, 0) == 0xFF0000FF && b.getPixel(1, 0) == 0xFFFFFFFF;
        });
    }

    /**
     * A module's resources as Google Play services' DynamiteLoader makes them on Android 11 and later: the platform's own
     * resources (getResourcesForApplication("android"), a Resources of their own, not the app's) with a ResourcesLoader of
     * the module's APK added -- here the APK of resources alone the app carries (assets/loader.apk, package
     * megler.test.loader, tools/build-test-app.sh). Its string is found by name in its package and read; its picture is
     * decoded by BitmapFactory.decodeResource, as the Google Maps module reads its textures; its ids are its package's; the
     * platform's own are still there; and the app's own resources do not have what the loader put over the platform's. (The
     * loader classes are Android 11's, not in the SDK the app is built against: stubs/, and addLoaders by reflection.)
     */
    private void resourcesLoader() {
        step("resources-loader", () -> {
            java.io.File apk = new java.io.File(getFilesDir(), "loader.apk");
            try (java.io.InputStream in = getAssets().open("loader.apk"); java.io.FileOutputStream out = new java.io.FileOutputStream(apk)) {
                byte[] buf = new byte[8192];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            android.content.res.Resources platform = getPackageManager().getResourcesForApplication("android");
            int before = platform.getIdentifier("loader_greeting", "string", "megler.test.loader");
            android.content.res.loader.ResourcesLoader loader = new android.content.res.loader.ResourcesLoader();
            try (android.os.ParcelFileDescriptor pfd = android.os.ParcelFileDescriptor.open(apk, android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
                loader.addProvider(android.content.res.loader.ResourcesProvider.loadFromApk(pfd));
            }
            android.content.res.Resources.class.getMethod("addLoaders", android.content.res.loader.ResourcesLoader[].class)
                    .invoke(platform, (Object) new android.content.res.loader.ResourcesLoader[] { loader });
            int id = platform.getIdentifier("loader_greeting", "string", "megler.test.loader");
            int markId = platform.getIdentifier("loader_mark", "drawable", "megler.test.loader");
            int colorId = platform.getIdentifier("loader_color", "color", "megler.test.loader");
            String text = id != 0 ? platform.getString(id) : null;
            String pkg = id != 0 ? platform.getResourcePackageName(id) : null;
            android.graphics.Bitmap mark = markId != 0 ? android.graphics.BitmapFactory.decodeResource(platform, markId) : null;
            int color = colorId != 0 ? platform.getColor(colorId, null) : 0;
            String own;
            try {
                own = getResources().getString(id);
            } catch (android.content.res.Resources.NotFoundException e) {
                own = "not found";
            }
            String ok = platform.getString(android.R.string.ok);
            detail = "before=" + before + " id=0x" + Integer.toHexString(id) + " text=" + text + " package=" + pkg + " mark=" + (mark == null ? "null" : mark.getWidth() + "x" + mark.getHeight() + " " + Integer.toHexString(mark.getPixel(1, 1)))
                    + " color=" + Integer.toHexString(color) + " own=" + own + " ok=" + ok + " same=" + (platform == getResources());
            return before == 0 && id != 0 && (id >>> 24) == 0x7f && "from the loader".equals(text) && "megler.test.loader".equals(pkg)
                    && mark != null && mark.getWidth() == 4 && mark.getPixel(1, 1) == 0xFF102030 && color == 0xFF102030
                    && !"from the loader".equals(own) && ok != null && !ok.isEmpty() && platform != getResources();
        });
    }

    /**
     * A switch bar as Android's Settings library makes one (MainSwitchBar, the head of Google Play services' Location accuracy
     * screen): a LinearLayout that inflates res/layout/switch_bar.xml into itself and finds its title, its switch and -- told
     * SDK_INT 31 or more -- the frame round them that layout-v31 and -v33 have and layout has not. Android 14 chooses -v33's,
     * whose title has three lines at most. The frame's sizes are dimensions of values-v31 alone, its background a selector of
     * drawable-v31 alone, the title's style a style of values-v31 alone: all there. The bar's setEnabled and setChecked set the
     * frame's state as MainSwitchBar's do; where the frame was missing, Google Play services ended on a NullPointerException
     * in MainSwitchBar.setEnabled. The app's other resources are still chosen at Android 9's level: the title's margin, a
     * dimension of values (24dp) and of values-v31 (16dp), is values'. And an XML document of xml and xml-v31 is -v31's.
     */
    private void layoutApiLevel() {
        step("layout-api-level", () -> {
            SwitchBar bar = new SwitchBar(this);
            android.view.View frame = bar.frame;
            android.widget.TextView title = bar.title;
            android.widget.Switch sw = bar.sw;
            if (frame == null || title == null || sw == null) {
                detail = "frame " + frame + " title " + title + " switch " + sw;
                return false;
            }
            bar.setEnabled(false);
            boolean disabled = !frame.isEnabled() && !title.isEnabled() && !sw.isEnabled();
            bar.setEnabled(true);
            android.graphics.drawable.Drawable bg = frame.getBackground();
            bar.setChecked(true);
            android.graphics.drawable.Drawable on = bg == null ? null : bg.getCurrent();
            boolean activated = frame.isActivated();
            bar.setChecked(false);
            android.graphics.drawable.Drawable off = bg == null ? null : bg.getCurrent();
            float density = getResources().getDisplayMetrics().density;
            int titleMargin = ((android.view.ViewGroup.MarginLayoutParams) title.getLayoutParams()).getMarginEnd();
            String level = null;
            try (android.content.res.XmlResourceParser p = getResources().getXml(R.xml.level_probe)) {
                for (int t = p.next(); t != org.xmlpull.v1.XmlPullParser.END_DOCUMENT; t = p.next()) {
                    if (t == org.xmlpull.v1.XmlPullParser.START_TAG) { level = p.getAttributeValue(null, "level"); break; }
                }
            }
            detail = "SDK_INT " + android.os.Build.VERSION.SDK_INT + ", title lines " + title.getMaxLines() + ", disabled " + disabled
                    + ", background " + bg + " on " + on + " off " + off + " activated " + activated + ", min height " + frame.getMinimumHeight()
                    + " padding " + frame.getPaddingStart() + " title margin " + titleMargin + " density " + density + ", xml level " + level;
            return android.os.Build.VERSION.SDK_INT >= 31 && frame.getId() == R.id.frame && title.getMaxLines() == 3 && disabled
                    && bg instanceof android.graphics.drawable.StateListDrawable && on != null && off != null && on != off && activated
                    && frame.getMinimumHeight() == Math.round(72 * density) && frame.getPaddingStart() == Math.round(20 * density)
                    && titleMargin == Math.round(24 * density) && "31".equals(level);
        });
    }

    /**
     * Resources of the app's own over the implementation of the activity's, as Tuya Smart's dynamic strings make them for every
     * activity (DynamicResourceContextThemeWrapper.getResources) and other libraries on Android 7 and later: getImpl of the
     * activity's Resources, then setImpl of that implementation on Resources made with the hidden Resources(ClassLoader), and on
     * a subclass made with the public constructor that answers one string itself -- hidden API, by reflection. Both then have
     * the activity's AssetManager, configuration and metrics (the very objects) and its resources: strings, a dimension, a
     * drawable, a platform colour, a layout inflated through a ContextThemeWrapper whose getResources gives the subclass (as
     * Tuya's wrapper), whose theme is made of it; the subclass's own string is its own. A theme the subclass made before, while
     * it had a night configuration of its own (ProbeTheme of values-night), is rebased onto the implementation it is given
     * (ProbeTheme of values). And ResourcesImpl's getters, Resources' field mResourcesImpl, the class loader given, the loaders.
     */
    private void resourcesImpl() {
        step("resources-impl", () -> {
            final android.content.res.Resources base = getResources();
            java.lang.reflect.Method getImpl = android.content.res.Resources.class.getDeclaredMethod("getImpl");
            getImpl.setAccessible(true);
            Object impl = getImpl.invoke(base);
            Class<?> implClass = Class.forName("android.content.res.ResourcesImpl");
            java.lang.reflect.Method setImpl = android.content.res.Resources.class.getDeclaredMethod("setImpl", implClass);
            setImpl.setAccessible(true);
            java.lang.reflect.Constructor<android.content.res.Resources> withLoader = android.content.res.Resources.class.getDeclaredConstructor(ClassLoader.class);
            withLoader.setAccessible(true);
            android.content.res.Resources plain = withLoader.newInstance(getClassLoader());
            setImpl.invoke(plain, impl);
            android.content.res.Configuration night = new android.content.res.Configuration(base.getConfiguration());
            night.uiMode = (night.uiMode & ~android.content.res.Configuration.UI_MODE_NIGHT_MASK) | android.content.res.Configuration.UI_MODE_NIGHT_YES;
            final DynamicStrings dynamic = new DynamicStrings(base.getAssets(), base.getDisplayMetrics(), night);
            Object own = getImpl.invoke(dynamic);
            android.content.res.Resources.Theme early = dynamic.newTheme();
            early.applyStyle(R.style.ProbeTheme, true);
            int before = foreground(early);
            setImpl.invoke(dynamic, impl);
            int rebased = foreground(early);
            android.content.res.Resources.Theme day = base.newTheme();
            day.applyStyle(R.style.ProbeTheme, true);
            // As Tuya's wrapper: a ContextThemeWrapper whose getResources gives the subclass, its theme made of it.
            android.view.ContextThemeWrapper wrapper = new android.view.ContextThemeWrapper(this, android.R.style.Theme_Material_Light) {
                @Override public android.content.res.Resources getResources() { return dynamic; }
            };
            android.view.View inflated = android.view.LayoutInflater.from(wrapper).inflate(R.layout.switch_bar, null);
            java.lang.reflect.Field field = android.content.res.Resources.class.getDeclaredField("mResourcesImpl");
            field.setAccessible(true);
            Object loaders = android.content.res.Resources.class.getMethod("getLoaders").invoke(plain);
            Object loader = android.content.res.Resources.class.getMethod("getClassLoader").invoke(plain);
            String name = base.getString(R.string.app_name);
            detail = "impl " + impl + " (" + (impl == null ? null : impl.getClass().getName()) + "), own " + own + "; strings " + plain.getString(R.string.app_name)
                    + " / " + dynamic.getString(R.string.app_name) + " / " + wrapper.getString(R.string.app_name) + "; foreground before " + Integer.toHexString(before)
                    + " rebased " + Integer.toHexString(rebased) + " day " + Integer.toHexString(foreground(day)) + "; inflated frame "
                    + (inflated == null ? null : inflated.findViewById(R.id.frame)) + "; loaders " + loaders + " class loader " + loader;
            return impl != null && impl.getClass() == implClass && getImpl.invoke(plain) == impl && getImpl.invoke(dynamic) == impl && own != null && own != impl
                    && field.get(base) == impl && field.get(dynamic) == impl
                    && plain.getAssets() == base.getAssets() && plain.getConfiguration() == base.getConfiguration() && plain.getDisplayMetrics() == base.getDisplayMetrics()
                    && dynamic.getConfiguration() == base.getConfiguration()
                    && implClass.getMethod("getAssets").invoke(impl) == base.getAssets() && implClass.getMethod("getConfiguration").invoke(impl) == base.getConfiguration()
                    && implClass.getMethod("getDisplayMetrics").invoke(impl) == base.getDisplayMetrics()
                    && name.equals(plain.getString(R.string.app_name)) && "Dynamic".equals(dynamic.getString(R.string.app_name)) && "Dynamic".equals(wrapper.getString(R.string.app_name))
                    && base.getString(R.string.sync_account_type).equals(dynamic.getString(R.string.sync_account_type))
                    && dynamic.getDimensionPixelSize(R.dimen.switch_title_margin) == base.getDimensionPixelSize(R.dimen.switch_title_margin)
                    && plain.getDrawable(R.drawable.inset_fraction, null) instanceof android.graphics.drawable.InsetDrawable
                    && plain.getColor(android.R.color.white, null) == 0xFFFFFFFF
                    && wrapper.getTheme().getResources() == dynamic && inflated != null && inflated.findViewById(R.id.frame) != null
                    && before == 0xFF222222 && rebased == 0xFF111111 && foreground(day) == 0xFF111111
                    && loaders instanceof java.util.List && ((java.util.List<?>) loaders).isEmpty() && loader == getClassLoader();
        });
    }

    /**
     * The process's Application as code that has no Context finds it, by Android's hidden ActivityThread: its field
     * sCurrentActivityThread, which currentActivityThread() gives too; that one's mInitialApplication -- which Tuya Smart's
     * library loader reads on a worker thread while its Application starts, and found no such field -- mAllApplications and
     * getApplication(); currentApplication() and AppGlobals.getInitialApplication(). All are the app's Application, here and
     * on the thread DataApplication.onCreate started, as Android sets them before the Application's onCreate.
     */
    private void activityThreadApplication() {
        step("activity-thread-application", () -> {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Field current = at.getDeclaredField("sCurrentActivityThread");
            current.setAccessible(true);
            java.lang.reflect.Field initial = at.getDeclaredField("mInitialApplication");
            initial.setAccessible(true);
            java.lang.reflect.Field all = at.getDeclaredField("mAllApplications");
            all.setAccessible(true);
            android.app.Application app = getApplication();
            Object fromField = initial.get(thread);
            java.util.List<?> apps = (java.util.List<?>) all.get(thread);
            Object fromGetter = at.getMethod("getApplication").invoke(thread);
            Object fromCurrent = at.getMethod("currentApplication").invoke(null);
            Object fromGlobals = Class.forName("android.app.AppGlobals").getMethod("getInitialApplication").invoke(null);
            detail = "application " + app + ", in its onCreate: " + DataApplication.seenInOnCreate + "; field " + fromField + ", all " + apps
                    + ", getApplication " + fromGetter + ", currentApplication " + fromCurrent + ", AppGlobals " + fromGlobals;
            return app instanceof DataApplication && "field true current true".equals(DataApplication.seenInOnCreate) && current.get(null) == thread
                    && fromField == app && apps != null && apps.contains(app) && fromGetter == app && fromCurrent == app && fromGlobals == app;
        });
        // The hidden ServiceManager, which Shizuku's SystemServiceHelper reaches by reflection for the system's binders (Sui asks it
        // for "package"): there (Morphe Manager ended at its start without the class), the platform's own services each a binder
        // that stands for it -- the same one each time, its interface's descriptor, alive, no local interface, every transaction
        // refused -- and any other name a service that is not there. Sui's bridge, a transaction of its own code on the binder,
        // brings no binder back: no Sui.
        step("service-manager", () -> {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            java.lang.reflect.Method get = sm.getMethod("getService", String.class);
            android.os.IBinder pkg = (android.os.IBinder) get.invoke(null, "package"), again = (android.os.IBinder) get.invoke(null, "package");
            android.os.IBinder checked = (android.os.IBinder) sm.getMethod("checkService", String.class).invoke(null, "activity");
            Object none = get.invoke(null, "megler.test.none");
            String[] listed = (String[]) sm.getMethod("listServices").invoke(null);
            Object orThrow = sm.getMethod("getServiceOrThrow", String.class).invoke(null, "package");
            boolean threw = false;
            try {
                sm.getMethod("getServiceOrThrow", String.class).invoke(null, "megler.test.none");
            } catch (java.lang.reflect.InvocationTargetException e) {
                threw = "android.os.ServiceManager$ServiceNotFoundException".equals(e.getCause().getClass().getName());
            }
            boolean answered = true;
            android.os.IBinder bridged = null;
            if (pkg != null) {
                android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
                data.writeInterfaceToken("android.content.pm.IPackageManager");
                data.writeInt(2);
                answered = pkg.transact(('_' << 24) | ('S' << 16) | ('U' << 8) | 'I', data, reply, 0);
                reply.readException();
                bridged = reply.readStrongBinder();
                data.recycle();
                reply.recycle();
            }
            java.util.List<String> names = listed == null ? java.util.Collections.<String>emptyList() : java.util.Arrays.asList(listed);
            detail = "getService " + (pkg == null ? null : pkg.getInterfaceDescriptor()) + " the same twice " + (pkg == again)
                    + ", checkService " + (checked == null ? null : checked.getInterfaceDescriptor()) + ", an unknown name " + none
                    + ", listServices " + names + ", getServiceOrThrow " + (orThrow == pkg) + " and threw for an unknown name " + threw
                    + ", a transaction of Sui's answered " + answered + " with " + bridged;
            return pkg != null && pkg == again && pkg == orThrow && pkg.pingBinder() && pkg.isBinderAlive()
                    && "android.content.pm.IPackageManager".equals(pkg.getInterfaceDescriptor())
                    && pkg.queryLocalInterface("android.content.pm.IPackageManager") == null
                    && checked != null && "android.app.IActivityManager".equals(checked.getInterfaceDescriptor())
                    && none == null && threw && names.contains("window") && names.contains("package") && names.contains("activity")
                    && !names.contains("megler.test.none") && !answered && bridged == null;
        });
        // Instagram's WindowInsetsManager (453), when the navigation bar's inset is 0 (the user has hidden the phone's bar), asks
        // the window manager whether there is a navigation bar: getService("window") cast to IBinder as Kotlin casts (null is a
        // NullPointerException), then IWindowManager.Stub.asInterface and hasNavigationBar() by reflection, and the platform's
        // config_showNavigationBar where the reflection fails with one of the exceptions it catches. A null binder ended
        // Instagram from its insets listener (0.1.0.681).
        step("service-manager-window", () -> {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            Object window = sm.getMethod("getService", String.class).invoke(sm, "window");
            if (window == null) throw new NullPointerException("null cannot be cast to non-null type android.os.IBinder");
            android.os.IBinder binder = (android.os.IBinder) window;
            Boolean has;
            String how;
            try {
                Class<?> stub = Class.forName("android.view.IWindowManager$Stub");
                Object wm = stub.getMethod("asInterface", android.os.IBinder.class).invoke(stub, binder);
                has = (Boolean) wm.getClass().getMethod("hasNavigationBar").invoke(wm);
                how = "IWindowManager";
            } catch (ClassCastException | ClassNotFoundException | IllegalAccessException | IllegalArgumentException | NoSuchMethodException
                     | SecurityException | java.lang.reflect.InvocationTargetException e) {
                int id = getResources().getIdentifier("config_showNavigationBar", "bool", "android");
                has = id > 0 && getResources().getBoolean(id);
                how = "config_showNavigationBar (" + id + ") after " + e.getClass().getSimpleName();
            }
            detail = "window " + binder.getInterfaceDescriptor() + ", a navigation bar " + has + " from " + how;
            return "android.view.IWindowManager".equals(binder.getInterfaceDescriptor()) && has != null;
        });
        // DateUtils' ranges into a Formatter, as Transit words a route's times: the Formatter given, the range appended to it (it
        // answered null, and Transit's "Go" took toString() of that); and a duration in words (it answered null too).
        step("date-range", () -> {
            long start = 1762700000000L, end = start + 45 * 60000L;
            java.util.Formatter f = new java.util.Formatter(new StringBuilder("at "), java.util.Locale.US);
            java.util.Formatter back = android.text.format.DateUtils.formatDateRange(this, f, start, end,
                    android.text.format.DateUtils.FORMAT_SHOW_TIME | android.text.format.DateUtils.FORMAT_24HOUR, "UTC");
            String range = back == null ? null : back.toString();
            String plain = android.text.format.DateUtils.formatDateRange(this, start, end, android.text.format.DateUtils.FORMAT_SHOW_DATE);
            Object duration = android.text.format.DateUtils.class.getMethod("formatDuration", long.class).invoke(null, 90L * 60000L);
            detail = "range=" + range + " plain=" + plain + " duration=" + duration;
            return back == f && range != null && range.startsWith("at ") && range.contains("14:53") && range.contains("15:38")
                    && plain != null && !plain.isEmpty() && duration != null && duration.toString().length() > 0;
        });
    }

    /**
     * Dates as Android words them. Messenger's chat list formats an older chat's date with a medium date format's clone given
     * DateFormat.getBestDateTimePattern(locale, "MMMd") or (another year) "MMMd, yyyy", and today's with DateFormat.getTimeFormat;
     * the skeleton itself was the pattern, and a Russian phone read "мая20, 2023" where Android says "20 мая 2023 г.". The
     * patterns are written here from ICU's data as Android 9 has it (ICU 60, CLDR 32) for en-US and ru-RU in both runs; the
     * formats of the default locale -- DateFormat.format's letters, the names, DateUtils' dates and relative times -- for the
     * run's own, en-US in the first and ru-RU in the second. (The settings' 12/24 hours are Megler's default, 24.)
     */
    private void dateFormats() {
        final java.util.Locale us = java.util.Locale.US, ru = new java.util.Locale("ru", "RU");
        final boolean russian = "ru".equals(java.util.Locale.getDefault().getLanguage());
        step("date-patterns", () -> {
            final String[][] want = {
                { "MMMd", "MMM d", "d MMM" },
                { "MMMd, yyyy", "MMM d, yyyy", "d MMM yyyy 'г'." },
                { "yMMMd", "MMM d, y", "d MMM y 'г'." },
                { "MMMMd", "MMMM d", "d MMMM" },
                { "yMMMMd", "MMMM d, y", "d MMMM y 'г'." },
                // (ICU 60's Russian "Hm" is "H:mm".)
                { "Hm", "HH:mm", "H:mm" },
                { "hm", "h:mm a", "h:mm a" },
            };
            StringBuilder got = new StringBuilder();
            boolean held = true;
            for (String[] w : want) {
                String a = android.text.format.DateFormat.getBestDateTimePattern(us, w[0]), b = android.text.format.DateFormat.getBestDateTimePattern(ru, w[0]);
                got.append(w[0]).append(" = ").append(a).append(" | ").append(b).append("; ");
                held &= w[1].equals(a) && w[2].equals(b);
            }
            java.util.Calendar may = java.util.Calendar.getInstance();
            may.clear();
            may.set(2023, java.util.Calendar.MAY, 20, 14, 53);
            java.util.Calendar jan = (java.util.Calendar) may.clone();
            jan.set(java.util.Calendar.MONTH, java.util.Calendar.JANUARY);
            jan.set(java.util.Calendar.DAY_OF_MONTH, 5);
            String usLong = messengerDate(us, "MMMd, yyyy", may.getTime()), ruLong = messengerDate(ru, "MMMd, yyyy", may.getTime());
            String usShort = messengerDate(us, "MMMd", jan.getTime()), ruShort = messengerDate(ru, "MMMd", jan.getTime());
            got.append("formats ").append(usLong).append(" | ").append(ruLong).append(" | ").append(usShort).append(" | ").append(ruShort);
            held &= "May 20, 2023".equals(usLong) && "20 мая 2023 г.".equals(ruLong) && "Jan 5".equals(usShort) && "5 янв.".equals(ruShort);
            // The locale's own clock, and the order of a pattern's day, month and year: hidden, reached as apps reach them.
            java.lang.reflect.Method is24 = android.text.format.DateFormat.class.getMethod("is24HourLocale", java.util.Locale.class);
            java.lang.reflect.Method order = android.text.format.DateFormat.class.getMethod("getDateFormatOrder", String.class);
            boolean usClock = (Boolean) is24.invoke(null, us), ruClock = (Boolean) is24.invoke(null, ru);
            String dmy = new String((char[]) order.invoke(null, "dd.MM.y")), mdy = new String((char[]) order.invoke(null, "M/d/yy 'г'."));
            got.append("; 24 hours ").append(usClock).append(" | ").append(ruClock).append("; order ").append(dmy).append(" | ").append(mdy);
            held &= !usClock && ruClock && "dMy".equals(dmy) && "Mdy".equals(mdy);
            detail = got.toString();
            Log.i("DataTest", "date-patterns " + detail);
            return held;
        });
        step("date-format-letters", () -> {
            java.util.Calendar when = java.util.Calendar.getInstance();
            when.clear();
            when.set(2023, java.util.Calendar.MAY, 20, 14, 53, 7); // a Saturday
            CharSequence full = android.text.format.DateFormat.format("EEEE, dd MMMM yyyy, kk:mm:ss", when);
            CharSequence alone = android.text.format.DateFormat.format("LLLL cccc", when);
            CharSequence quoted = android.text.format.DateFormat.format("'at' h:mm, ''yy", when);
            CharSequence spanned = android.text.format.DateFormat.format(new android.text.SpannedString("d.M.yy"), when);
            CharSequence plain = android.text.format.DateFormat.format("d.M.yy", when);
            String month = android.text.format.DateUtils.getMonthString(java.util.Calendar.JANUARY, android.text.format.DateUtils.LENGTH_LONG);
            String mon = android.text.format.DateUtils.getMonthString(java.util.Calendar.JANUARY, android.text.format.DateUtils.LENGTH_MEDIUM);
            String day = android.text.format.DateUtils.getDayOfWeekString(java.util.Calendar.SATURDAY, android.text.format.DateUtils.LENGTH_LONG);
            String dy = android.text.format.DateUtils.getDayOfWeekString(java.util.Calendar.SATURDAY, android.text.format.DateUtils.LENGTH_MEDIUM);
            String pm = android.text.format.DateUtils.getAMPMString(java.util.Calendar.PM);
            // The clock: the settings' 24 hours, then 12 asked for, then 24 again.
            boolean clock24 = android.text.format.DateFormat.is24HourFormat(this);
            String time24 = ((java.text.SimpleDateFormat) android.text.format.DateFormat.getTimeFormat(this)).toPattern();
            String shown24 = android.text.format.DateFormat.getTimeFormat(this).format(when.getTime());
            android.provider.Settings.System.putString(getContentResolver(), android.provider.Settings.System.TIME_12_24, "12");
            boolean clock12 = android.text.format.DateFormat.is24HourFormat(this);
            String time12 = ((java.text.SimpleDateFormat) android.text.format.DateFormat.getTimeFormat(this)).toPattern();
            android.provider.Settings.System.putString(getContentResolver(), android.provider.Settings.System.TIME_12_24, "24");
            detail = "full=" + full + " alone=" + alone + " quoted=" + quoted + " spanned=" + spanned + " (" + spanned.getClass().getSimpleName() + ") plain="
                    + plain + " (" + plain.getClass().getSimpleName() + ") month=" + month + " mon=" + mon + " day=" + day + " dy=" + dy + " pm=" + pm
                    + " clock " + clock24 + " " + time24 + " " + shown24 + ", 12: " + clock12 + " " + time12;
            Log.i("DataTest", "date-format-letters " + detail);
            boolean names = russian
                    ? "суббота, 20 мая 2023, 14:53:07".equals(full.toString()) && "май суббота".equals(alone.toString()) && "января".equals(month)
                        && "янв.".equals(mon) && "суббота".equals(day) && "сб".equals(dy)
                    : "Saturday, 20 May 2023, 14:53:07".equals(full.toString()) && "May Saturday".equals(alone.toString()) && "January".equals(month)
                        && "Jan".equals(mon) && "Saturday".equals(day) && "Sat".equals(dy) && "PM".equals(pm);
            return names && "at 2:53, '23".equals(quoted.toString()) && spanned instanceof android.text.Spanned && "20.5.23".equals(spanned.toString())
                    && plain instanceof String && "20.5.23".equals(plain) && clock24 && (russian ? "H:mm" : "HH:mm").equals(time24) && "14:53".equals(shown24)
                    && !clock12 && "h:mm a".equals(time12) && android.text.format.DateFormat.is24HourFormat(this);
        });
        step("date-utils-locale", () -> {
            java.util.Calendar when = java.util.Calendar.getInstance();
            when.clear();
            when.set(2023, java.util.Calendar.MAY, 20, 14, 53);
            long t = when.getTimeInMillis();
            String medium = android.text.format.DateUtils.formatDateTime(this, t, android.text.format.DateUtils.FORMAT_SHOW_DATE
                    | android.text.format.DateUtils.FORMAT_SHOW_YEAR | android.text.format.DateUtils.FORMAT_ABBREV_MONTH);
            String numeric = android.text.format.DateUtils.formatDateTime(this, t, android.text.format.DateUtils.FORMAT_SHOW_DATE
                    | android.text.format.DateUtils.FORMAT_SHOW_YEAR | android.text.format.DateUtils.FORMAT_NUMERIC_DATE);
            String time = android.text.format.DateUtils.formatDateTime(this, t, android.text.format.DateUtils.FORMAT_SHOW_TIME);
            long now = System.currentTimeMillis(), minute = android.text.format.DateUtils.MINUTE_IN_MILLIS, hour = android.text.format.DateUtils.HOUR_IN_MILLIS;
            CharSequence ago = android.text.format.DateUtils.getRelativeTimeSpanString(now - 5 * minute, now, minute);
            CharSequence ahead = android.text.format.DateUtils.getRelativeTimeSpanString(now + 3 * hour, now, minute);
            CharSequence older = android.text.format.DateUtils.getRelativeTimeSpanString(t, now, minute);
            CharSequence on = android.text.format.DateUtils.getRelativeTimeSpanString(this, t, true);
            // "[relative], [time]": the time in the locale's own clock; a day before when the three hours cross midnight.
            long then = now - 3 * hour;
            CharSequence both = android.text.format.DateUtils.getRelativeDateTimeString(this, then, minute, android.text.format.DateUtils.WEEK_IN_MILLIS, 0);
            java.util.Calendar a = java.util.Calendar.getInstance(), b = java.util.Calendar.getInstance();
            a.setTimeInMillis(then);
            b.setTimeInMillis(now);
            boolean crossed = a.get(java.util.Calendar.DAY_OF_YEAR) != b.get(java.util.Calendar.DAY_OF_YEAR);
            String clock = new java.text.SimpleDateFormat(russian ? "H:mm" : "h:mm a", java.util.Locale.getDefault()).format(new java.util.Date(then));
            String relative = crossed ? (russian ? "Вчера" : "Yesterday") : (russian ? "3 часа назад" : "3 hours ago");
            detail = "medium=" + medium + " numeric=" + numeric + " time=" + time + " ago=" + ago + " ahead=" + ahead + " older=" + older + " on=" + on + " both=" + both;
            Log.i("DataTest", "date-utils-locale " + detail);
            boolean held = russian
                    ? "20 мая 2023 г.".equals(medium) && "20.05.2023".equals(numeric) && "5 минут назад".equals(ago.toString())
                        && "Через 3 часа".equals(ahead.toString()) && "20 мая 2023 г.".equals(older.toString()) && "20.05.2023".equals(on.toString())
                    : "May 20, 2023".equals(medium) && "5/20/2023".equals(numeric) && "5 minutes ago".equals(ago.toString())
                        && "In 3 hours".equals(ahead.toString()) && "May 20, 2023".equals(older.toString()) && "on 5/20/2023".equals(on.toString());
            return held && "14:53".equals(time) && both.toString().startsWith(relative + ", ") && both.toString().endsWith(clock);
        });
    }

    /** A date as Messenger formats its chat list's: a medium date format of the locale, cloned, given the skeleton's best pattern. */
    private static String messengerDate(java.util.Locale locale, String skeleton, java.util.Date when) {
        java.text.SimpleDateFormat f = (java.text.SimpleDateFormat) java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM, locale).clone();
        f.applyPattern(android.text.format.DateFormat.getBestDateTimePattern(locale, skeleton));
        return f.format(when);
    }

    /**
     * A ShapeDrawable of a RoundRectShape with an inset of nothing, as Messenger's rounded backgrounds are made (its search
     * field's): the rounded rectangle filled whole, nothing outside its curves. An inset that is smaller both ways is a hole.
     * The inset of nothing cut the shape out of its bounds, and the corners alone were filled: a black pill with grey corners.
     * And the outlines as Android's: none for a shape with an inset, the rounded rectangle for one without.
     */
    private void roundRectShape() {
        step("round-rect-shape", () -> {
            float[] radii = { 20, 20, 20, 20, 20, 20, 20, 20 };
            android.graphics.drawable.ShapeDrawable pill = new android.graphics.drawable.ShapeDrawable(
                    new android.graphics.drawable.shapes.RoundRectShape(radii, new android.graphics.RectF(), null));
            pill.mutate();
            pill.setColorFilter(0xFF3A3B3C, android.graphics.PorterDuff.Mode.SRC_IN);
            pill.setBounds(0, 0, 100, 40);
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(100, 40, android.graphics.Bitmap.Config.ARGB_8888);
            pill.draw(new android.graphics.Canvas(bm));
            int centre = bm.getPixel(50, 20), corner = bm.getPixel(1, 1), end = bm.getPixel(2, 20), top = bm.getPixel(50, 1);
            android.graphics.drawable.ShapeDrawable ring = new android.graphics.drawable.ShapeDrawable(
                    new android.graphics.drawable.shapes.RoundRectShape(radii, new android.graphics.RectF(10, 10, 10, 10), null));
            ring.getPaint().setColor(0xFF00FF00);
            ring.setBounds(0, 0, 100, 40);
            android.graphics.Bitmap b2 = android.graphics.Bitmap.createBitmap(100, 40, android.graphics.Bitmap.Config.ARGB_8888);
            ring.draw(new android.graphics.Canvas(b2));
            int hole = b2.getPixel(50, 20), band = b2.getPixel(50, 4);
            // The outlines, Android's: none for a shape with an inset (even one of nothing), the rounded rectangle for one without.
            android.graphics.Outline insetOutline = new android.graphics.Outline();
            pill.getOutline(insetOutline);
            android.graphics.drawable.ShapeDrawable plain = new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.RoundRectShape(radii, null, null));
            plain.setBounds(0, 0, 100, 40);
            android.graphics.Outline outline = new android.graphics.Outline();
            plain.getOutline(outline);
            android.graphics.Rect box = new android.graphics.Rect();
            boolean rounded = outline.getRect(box);
            detail = String.format("centre=%08x corner=%08x end=%08x top=%08x hole=%08x band=%08x inset's outline empty %b, outline %b %s radius %.1f alpha %.2f",
                    centre, corner, end, top, hole, band, insetOutline.isEmpty(), rounded, box.toShortString(), outline.getRadius(), outline.getAlpha());
            Log.i("DataTest", "round-rect-shape " + detail);
            return centre == 0xFF3A3B3C && (corner >>> 24) == 0 && end == 0xFF3A3B3C && top == 0xFF3A3B3C && (hole >>> 24) == 0 && band == 0xFF00FF00
                    && insetOutline.isEmpty() && rounded && box.equals(new android.graphics.Rect(0, 0, 100, 40)) && outline.getRadius() == 20 && outline.getAlpha() == 1;
        });
    }

    /**
     * Whether a view is shown, heard by its drawables as on Android (View.onVisibilityAggregated, API 24): an ImageView's
     * picture and a view's background visible once the window has them, not once their parent is made GONE, visible again
     * with it, and not once their parent leaves the window. Megler never said it: Messenger's network drawables were never
     * hidden ("AsyncDrawable ... wasn't hidden before it was GC'd"). Said after the window is attached (the steps run before),
     * on its own line.
     */
    private void visibilityAggregated() {
        step("visibility-aggregated", () -> {
            final StringBuilder heard = new StringBuilder();
            final android.graphics.drawable.ColorDrawable picture = new android.graphics.drawable.ColorDrawable(0xFF0000FF);
            final android.graphics.drawable.ColorDrawable back = new android.graphics.drawable.ColorDrawable(0xFF00FF00);
            final android.widget.FrameLayout outer = new android.widget.FrameLayout(this);
            final android.widget.ImageView image = new android.widget.ImageView(this);
            image.setImageDrawable(picture);
            final android.view.View plain = new android.view.View(this);
            plain.setBackground(back);
            outer.addView(image, new android.widget.FrameLayout.LayoutParams(10, 10));
            outer.addView(plain, new android.widget.FrameLayout.LayoutParams(10, 10));
            outer.addOnAttachStateChangeListener(new android.view.View.OnAttachStateChangeListener() {
                @Override public void onViewAttachedToWindow(android.view.View v) {
                    outer.removeOnAttachStateChangeListener(this);
                    outer.post(() -> {
                        heard.append("attached ").append(picture.isVisible()).append(' ').append(back.isVisible());
                        outer.setVisibility(android.view.View.GONE);
                        heard.append(", gone ").append(picture.isVisible()).append(' ').append(back.isVisible());
                        outer.setVisibility(android.view.View.VISIBLE);
                        heard.append(", visible ").append(picture.isVisible()).append(' ').append(back.isVisible());
                        ((android.view.ViewGroup) outer.getParent()).removeView(outer);
                        heard.append(", removed ").append(picture.isVisible()).append(' ').append(back.isVisible());
                        Log.i("DataTest", "visibility-aggregated " + heard);
                    });
                }
                @Override public void onViewDetachedFromWindow(android.view.View v) {}
            });
            ((android.view.ViewGroup) text.getParent()).addView(outer, new android.view.ViewGroup.LayoutParams(20, 20));
            detail = "in the window now " + outer.isAttachedToWindow();
            return true;
        });
    }

    /**
     * A view made on a worker thread with a state list for its background, as Facebook (583) builds its tab bar's tabs in a
     * Callable on an executor of its own: the view's invalidateDrawable asserts the main thread, as Facebook's does (its
     * DefaultAndroidThreadUtil.assertOnUiThread). Android makes the view the background's callback after the background's
     * state is set (View.setBackgroundDrawable: "Set callback last"), so the state list choosing its child for the view's
     * state, and the child's gradient taking that state's colour, reach no view, and setBackground passes on the worker; the
     * view is the callback afterwards, and a change of its state on the main thread reaches it. The view as the callback
     * first ended Facebook as its main tabs came up ("This operation must be run on UI thread.", 0.1.0.681).
     */
    private void backgroundOffMainThread() {
        step("background-callback-last", () -> {
            final android.graphics.drawable.GradientDrawable[] child = new android.graphics.drawable.GradientDrawable[1];
            final int[] heard = new int[1];
            java.util.concurrent.FutureTask<android.view.View> made = new java.util.concurrent.FutureTask<>(() -> {
                android.view.View tab = new android.view.View(this) {
                    @Override
                    public void invalidateDrawable(android.graphics.drawable.Drawable d) {
                        super.invalidateDrawable(d);
                        if (android.os.Looper.myLooper() != android.os.Looper.getMainLooper()) throw new IllegalStateException("This operation must be run on UI thread.");
                        heard[0]++;
                    }
                };
                android.graphics.drawable.GradientDrawable enabled = new android.graphics.drawable.GradientDrawable();
                enabled.setColor(new android.content.res.ColorStateList(new int[][] { { android.R.attr.state_enabled }, {} }, new int[] { 0xFF1877F2, 0xFF65676B }));
                child[0] = enabled;
                android.graphics.drawable.StateListDrawable states = new android.graphics.drawable.StateListDrawable();
                states.addState(new int[] { android.R.attr.state_enabled }, enabled);
                states.addState(new int[0], new android.graphics.drawable.ColorDrawable(0xFF000000));
                states.setExitFadeDuration(150);
                tab.setBackground(states);
                return tab;
            });
            new Thread(made, "tab-maker").start();
            android.view.View tab;
            try {
                tab = made.get(10, java.util.concurrent.TimeUnit.SECONDS);
            } catch (java.util.concurrent.ExecutionException e) {
                detail = "setBackground on the worker thread threw " + e.getCause();
                return false;
            }
            android.graphics.drawable.Drawable bg = tab.getBackground();
            boolean chose = bg != null && bg.getCurrent() == child[0], callback = bg != null && bg.getCallback() == tab;
            int before = heard[0];
            tab.setEnabled(false);
            detail = "the enabled child chosen " + chose + ", the view the callback " + callback + ", invalidateDrawable heard "
                    + before + " times on the worker, " + heard[0] + " after the view was disabled on the main thread";
            return chose && callback && before == 0 && heard[0] > 0;
        });
    }

    /**
     * A search result's title as Transit 6.3.7 shows one (its util class e, d(), for each row of the results' adapter): the
     * markup the app's native code writes -- "<color|light/dark>" before the words, a light theme's colour and a dark one's --
     * stays in the text, hidden by a ReplacementSpan of no width that draws nothing, and the text from there is coloured by a
     * MetricAffectingSpan of the app's own; set with BufferType.SPANNABLE on a TextView that is android:singleLine
     * (res/layout/markup_row.xml, as the app's card row). Android shows the words alone, in the colour. The singleLine's
     * SingleLineTransformationMethod gave the view a String of the text, every span gone: the markup was drawn, in the view's
     * own colour, and ran out of the row (0.1.0.684, a Lumia 650). And what that transformation gives for each kind of text, as
     * Android's: the text itself where it has no line break, the spans kept where it has, an Editable followed as it changes.
     */
    private void singleLineSpans() {
        step("single-line-spans", () -> {
            android.widget.TextView row = (android.widget.TextView) android.view.LayoutInflater.from(this).inflate(R.layout.markup_row, null);
            android.text.method.TransformationMethod how = row.getTransformationMethod();
            String markup = "<color|ff0000/00ff00>", words = "Choose on map";
            android.text.SpannableString text = new android.text.SpannableString(markup + words);
            text.setSpan(new android.text.style.MetricAffectingSpan() {
                @Override
                public void updateDrawState(android.text.TextPaint p) {
                    p.setColor(0xFFFF0000);
                }

                @Override
                public void updateMeasureState(android.text.TextPaint p) {
                    p.setColor(0xFFFF0000);
                }
            }, 0, text.length(), 0);
            text.setSpan(new android.text.style.ReplacementSpan() {
                @Override
                public int getSize(android.graphics.Paint p, CharSequence t, int start, int end, android.graphics.Paint.FontMetricsInt fm) {
                    return 0;
                }

                @Override
                public void draw(android.graphics.Canvas c, CharSequence t, int start, int end, float x, int top, int y, int bottom, android.graphics.Paint p) {
                }
            }, 0, markup.length(), 0);
            row.setText(text, android.widget.TextView.BufferType.SPANNABLE);
            int w = 300;
            row.measure(android.view.View.MeasureSpec.makeMeasureSpec(w, android.view.View.MeasureSpec.EXACTLY),
                        android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED));
            int h = Math.max(1, row.getMeasuredHeight());
            row.layout(0, 0, w, h);
            android.text.Layout l = row.getLayout();
            CharSequence shown = l == null ? null : l.getText();
            int hidden = shown instanceof android.text.Spanned ? ((android.text.Spanned) shown).getSpans(0, shown.length(), android.text.style.ReplacementSpan.class).length : 0;
            float width = l == null ? -1 : l.getLineWidth(0), alone = row.getPaint().measureText(words);
            int ellipsis = l == null ? -1 : l.getEllipsisCount(0);
            // The picture: the characters drawn red, each a run of red pixels along a row through them, and nothing black.
            android.graphics.Bitmap b = bitmap(w, h);
            android.graphics.Canvas c = new android.graphics.Canvas(b);
            c.drawColor(0xFFFFFFFF);
            row.draw(c);
            int runs = 0, dark = 0, left = w;
            for (int y = 0; y < h; y++) {
                int inRow = 0;
                boolean in = false;
                for (int x = 0; x < w; x++) {
                    int p = b.getPixel(x, y), r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, bl = p & 0xFF;
                    boolean red = r >= 128 && g < 128 && bl < 128;
                    if (red && !in) inRow++;
                    if (red) left = Math.min(left, x);
                    in = red;
                    if (r < 128 && g < 128 && bl < 128) dark++;
                }
                runs = Math.max(runs, inRow);
            }
            // The transformation alone.
            android.text.method.TransformationMethod one = android.text.method.SingleLineTransformationMethod.getInstance();
            boolean itself = one.getTransformation(text, row) == text;
            android.text.SpannableString lines = new android.text.SpannableString("a\nb\rc");
            Object bold = new android.text.style.StyleSpan(android.graphics.Typeface.BOLD);
            lines.setSpan(bold, 0, lines.length(), 0);
            CharSequence joined = one.getTransformation(lines, row);
            boolean kept = joined instanceof android.text.Spanned && "a b﻿c".equals(joined.toString())
                    && ((android.text.Spanned) joined).getSpanStart(bold) == 0 && ((android.text.Spanned) joined).getSpanEnd(bold) == 5;
            android.text.SpannableStringBuilder typed = new android.text.SpannableStringBuilder("x\ny");
            CharSequence follows = one.getTransformation(typed, row);
            typed.replace(0, 1, "z");
            CharSequence plain = one.getTransformation("p\nq", row);
            detail = "transformation " + (how == null ? null : how.getClass().getSimpleName()) + ", the layout's text "
                    + (shown == null ? null : shown.getClass().getSimpleName() + " \"" + shown + "\"") + " with " + hidden + " ReplacementSpan, its line "
                    + width + " px wide (the words alone " + alone + "), ellipsis " + ellipsis + "; drawn: " + runs + " red characters from x=" + left
                    + ", " + dark + " black pixels; the text itself back " + itself + ", lines joined with their span " + kept + " (\""
                    + joined + "\"), an Editable followed \"" + follows + "\", a String \"" + plain + "\" " + (plain instanceof String);
            return how instanceof android.text.method.SingleLineTransformationMethod && hidden == 1 && Math.abs(width - alone) < 0.5f && ellipsis == 0
                    && runs == 11 && left < 3 && dark == 0 && itself && kept && "z y".equals(follows.toString()) && plain instanceof String && "p q".equals(plain);
        });
    }

    /**
     * A single-line title with an ellipsis at its end measured wide and then narrower, as a LinearLayout not given an exact
     * width measures a weighted child: at its own width first, at its share after. Android makes the layout again when the
     * width it ellipsizes at changes, and the ellipsis is where the narrower width ends. The view kept the layout of its first,
     * wider measure -- a single line scrolls horizontally, and a layout wider than the view was taken to be as good -- so the
     * line had no ellipsis and was cut at the view's edge: Transit's search cards' titles ended "<color|1e212d/f0f0f1>Ch", no
     * ellipsis (0.1.0.684).
     */
    private void ellipsisNarrower() {
        step("ellipsis-narrower", () -> {
            android.widget.TextView title = (android.widget.TextView) android.view.LayoutInflater.from(this).inflate(R.layout.markup_row, null);
            title.setText("A title wider than its second measure");
            title.measure(android.view.View.MeasureSpec.makeMeasureSpec(500, android.view.View.MeasureSpec.EXACTLY),
                          android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED));
            android.text.Layout first = title.getLayout();
            int wide = first == null ? -1 : first.getEllipsisCount(0);
            title.measure(android.view.View.MeasureSpec.makeMeasureSpec(200, android.view.View.MeasureSpec.EXACTLY),
                          android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED));
            title.layout(0, 0, 200, Math.max(1, title.getMeasuredHeight()));
            android.text.Layout l = title.getLayout();
            int ellipsis = l == null ? -1 : l.getEllipsisCount(0), at = l == null ? -1 : l.getEllipsizedWidth();
            detail = "at 500 px the ellipsis over " + wide + " characters; at 200 px a layout " + (l == null ? null : l.getWidth()) + " px wide, ellipsized at "
                    + at + " px over " + ellipsis + " characters";
            return wide == 0 && ellipsis > 0 && at == 200;
        });
    }

    /**
     * The same markup as Transit's Compose screens read it (6.3.7, its class ui.c): a Kotlin Regex of a tag's name and thirteen
     * optional fields, each "\|?([^|>]*)?", then ">", found by Regex.find from where the last tag ended -- a new Matcher and
     * find(int) each time, the fields read by group(i) as Kotlin's groupValues reads them (an absent one ""). java.util.regex is
     * ICU's matcher here, as on Android: each tag found whole, the fields the text has between its bars.
     */
    private void markupRegex() {
        step("markup-regex", () -> {
            StringBuilder fields = new StringBuilder();
            for (int i = 0; i < 13; i++) fields.append("\\|?([^|>]*)?");
            java.util.regex.Pattern tag = java.util.regex.Pattern.compile("<(strong|small|light|color|image|date|realtime-symbol|link)" + fields + ">");
            String text = "<color|1e212d/f0f0f1>Choose on map <image|bus||-1|1|-1|-1|Center> 5<realtime-symbol|*0>";
            StringBuilder found = new StringBuilder();
            int tags = 0, groups = -1, from = 0;
            boolean whole = true;
            java.util.regex.Matcher m;
            while (from < text.length() && (m = tag.matcher(text)).find(from)) {
                tags++;
                groups = m.groupCount();
                int last = 1;
                for (int g = 2; g <= groups; g++) if (m.group(g) != null && !m.group(g).isEmpty()) last = g;
                StringBuilder again = new StringBuilder("<").append(m.group(1));
                for (int g = 2; g <= last; g++) again.append('|').append(m.group(g) == null ? "" : m.group(g));
                again.append('>');
                whole &= again.toString().equals(text.substring(m.start(), m.end()));
                found.append(m.start()).append('-').append(m.end()).append(' ').append(again).append("; ");
                from = m.end();
            }
            detail = tags + " tags, " + groups + " groups: " + found;
            return tags == 3 && groups == 14 && whole && found.toString().equals("0-21 <color|1e212d/f0f0f1>; 35-65 <image|bus||-1|1|-1|-1|Center>; 67-87 <realtime-symbol|*0>; ");
        });
    }

    /**
     * AudioAttributes equal by what they hold, as Android's: two built alike, and one and its copy through a Parcel (writeToParcel
     * and CREATOR), equal with the same hash; another usage, content type or flag not. They were equal to themselves alone, and a
     * NotificationChannel that came through a Parcel was never equal to its original.
     */
    private void audioAttributesEqual() {
        step("audio-attributes-equal", () -> {
            android.media.AudioAttributes a = new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_NOTIFICATION)
                    .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
            android.media.AudioAttributes b = new android.media.AudioAttributes.Builder().setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .setUsage(android.media.AudioAttributes.USAGE_NOTIFICATION).build();
            android.os.Parcel p = android.os.Parcel.obtain();
            a.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.media.AudioAttributes parcelled = android.media.AudioAttributes.CREATOR.createFromParcel(p);
            p.recycle();
            android.media.AudioAttributes otherUsage = new android.media.AudioAttributes.Builder(a).setUsage(android.media.AudioAttributes.USAGE_ALARM).build();
            android.media.AudioAttributes otherContent = new android.media.AudioAttributes.Builder(a).setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build();
            android.media.AudioAttributes otherFlags = new android.media.AudioAttributes.Builder(a).setFlags(android.media.AudioAttributes.FLAG_AUDIBILITY_ENFORCED).build();
            detail = "built alike " + a.equals(b) + " " + (a.hashCode() == b.hashCode()) + ", through a parcel " + (parcelled != a) + " " + a.equals(parcelled)
                     + " " + (a.hashCode() == parcelled.hashCode()) + ", another usage " + a.equals(otherUsage) + ", content " + a.equals(otherContent)
                     + ", flags " + a.equals(otherFlags) + ", null " + a.equals(null);
            return a.equals(b) && b.equals(a) && a.hashCode() == b.hashCode() && parcelled != a && a.equals(parcelled) && a.hashCode() == parcelled.hashCode()
                   && !a.equals(otherUsage) && !a.equals(otherContent) && !a.equals(otherFlags) && !a.equals(null) && !a.equals("AudioAttributes");
        });
    }

    /** A public method of Android's that android.jar (API 29) does not have, or a hidden one, called by reflection. */
    private static Object call(Object target, String name, Class<?>[] types, Object... args) throws ReflectiveOperationException {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }

    private static final Class<?>[] NO_TYPES = {};

    /**
     * A notification channel as Android's (11 to 14): Android's defaults where the app sets nothing -- the default notification
     * sound played as a notification's (Notification.AUDIO_ATTRIBUTES_DEFAULT), no lights, no vibration, a badge, no lock screen
     * visibility of its own -- and what the app sets kept, a silent channel's sound and attributes null among it. And Messenger's
     * copy of a channel of its own through the getters (X.30q.A00 of its version on a Lumia 650), which asserts the audio
     * attributes it gave the channel: they were null, every setter of the stub kept nothing, and Messenger ended ("INVOKE_RETURN
     * must not be null").
     */
    private void notificationChannel() {
        step("notification-channel", () -> {
            android.app.NotificationChannel d = new android.app.NotificationChannel("defaults", "Defaults", android.app.NotificationManager.IMPORTANCE_DEFAULT);
            android.media.AudioAttributes da = d.getAudioAttributes();
            String defaults = d.getSound() + " " + (da == null ? "null" : da.getUsage() + "/" + da.getContentType()) + " lights " + d.shouldShowLights() + " "
                    + d.getLightColor() + " vibrates " + d.shouldVibrate() + " " + Arrays.toString(d.getVibrationPattern()) + " badge " + d.canShowBadge()
                    + " lock screen " + d.getLockscreenVisibility() + " dnd " + d.canBypassDnd() + " bubbles " + d.canBubble() + " group " + d.getGroup()
                    + " description " + d.getDescription() + " set importance " + d.hasUserSetImportance() + " set sound " + call(d, "hasUserSetSound", NO_TYPES)
                    + " parent " + call(d, "getParentChannelId", NO_TYPES) + " conversation " + call(d, "getConversationId", NO_TYPES) + " "
                    + call(d, "isConversation", NO_TYPES) + " blockable " + call(d, "isBlockable", NO_TYPES);
            boolean defaulted = android.provider.Settings.System.DEFAULT_NOTIFICATION_URI.equals(d.getSound()) && da != null
                    && da.getUsage() == android.media.AudioAttributes.USAGE_NOTIFICATION && da.getContentType() == android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION
                    && defaults.endsWith(" lights false 0 vibrates false null badge true lock screen -1000 dnd false bubbles false group null description null"
                            + " set importance false set sound false parent null conversation null false blockable false")
                    && "defaults".equals(d.getId()) && "Defaults".equals(d.getName().toString()) && d.getImportance() == android.app.NotificationManager.IMPORTANCE_DEFAULT;
            // What the app sets is kept.
            android.net.Uri tone = android.net.Uri.parse("android.resource://megler.test.data/raw/tone");
            android.media.AudioAttributes ringtone = new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
            long[] pattern = { 0, 250, 250, 250 };
            android.app.NotificationChannel c = new android.app.NotificationChannel("set", "Set", android.app.NotificationManager.IMPORTANCE_HIGH);
            c.setSound(tone, ringtone);
            c.enableLights(true);
            c.setLightColor(0xFF00FF00);
            c.setVibrationPattern(pattern);
            c.setShowBadge(false);
            c.setLockscreenVisibility(android.app.Notification.VISIBILITY_SECRET);
            c.setBypassDnd(true);
            c.setAllowBubbles(true);
            c.setGroup("group");
            c.setDescription("described");
            c.setName("Renamed");
            c.setImportance(android.app.NotificationManager.IMPORTANCE_LOW);
            call(c, "setBlockable", new Class<?>[] { boolean.class }, true);
            call(c, "setConversationId", new Class<?>[] { String.class, String.class }, "set", "conversation");
            String set = c.getSound() + " " + (c.getAudioAttributes() == ringtone) + " lights " + c.shouldShowLights() + " " + Integer.toHexString(c.getLightColor())
                    + " vibrates " + c.shouldVibrate() + " " + (c.getVibrationPattern() == pattern) + " badge " + c.canShowBadge() + " lock screen "
                    + c.getLockscreenVisibility() + " dnd " + c.canBypassDnd() + " bubbles " + c.canBubble() + " group " + c.getGroup() + " description "
                    + c.getDescription() + " name " + c.getName() + " importance " + c.getImportance() + " blockable " + call(c, "isBlockable", NO_TYPES)
                    + " parent " + call(c, "getParentChannelId", NO_TYPES) + " conversation " + call(c, "getConversationId", NO_TYPES) + " "
                    + call(c, "isConversation", NO_TYPES);
            boolean kept = set.equals(tone + " true lights true ff00ff00 vibrates true true badge false lock screen -1 dnd true bubbles true group group"
                    + " description described name Renamed importance 2 blockable true parent set conversation conversation true");
            // A pattern of nothing, or none, turns the vibration off; enableVibration turns it on whatever the pattern.
            c.setVibrationPattern(new long[0]);
            boolean emptyOff = !c.shouldVibrate();
            c.enableVibration(true);
            boolean onAgain = c.shouldVibrate();
            c.setVibrationPattern(null);
            boolean noneOff = !c.shouldVibrate() && c.getVibrationPattern() == null;
            // A silent channel's: no sound and no attributes, given back as they were given.
            c.setSound(null, null);
            boolean silent = c.getSound() == null && c.getAudioAttributes() == null;
            // Messenger's model of a channel (X.2wl.A01 of its version 585) copied through the getters as its X.30q.A00 copies it.
            android.app.NotificationChannel model = new android.app.NotificationChannel("messenger_model", "Messages", android.app.NotificationManager.IMPORTANCE_HIGH);
            model.setGroup("messenger_group");
            model.enableLights(true);
            model.setLightColor(0xFF0084FF);
            model.setVibrationPattern(pattern);
            model.enableVibration(true);
            model.setSound(tone, ringtone);
            model.setShowBadge(true);
            android.app.NotificationChannel copied = new android.app.NotificationChannel(model.getId(), model.getName(), model.getImportance());
            copied.setGroup(model.getGroup());
            copied.enableLights(model.shouldShowLights());
            copied.setLightColor(model.getLightColor());
            copied.setVibrationPattern(model.getVibrationPattern());
            copied.enableVibration(model.shouldVibrate());
            copied.setSound(model.getSound(), java.util.Objects.requireNonNull(model.getAudioAttributes(), "INVOKE_RETURN must not be null"));
            copied.setShowBadge(model.canShowBadge());
            boolean messenger = copied.equals(model) && model.equals(copied) && copied.hashCode() == model.hashCode();
            detail = "defaults: " + defaults + "; set: " + set + "; a pattern of nothing off " + emptyOff + ", on " + onAgain + ", none off " + noneOff
                     + "; silent " + silent + "; Messenger's copy equal " + messenger;
            return defaulted && kept && emptyOff && onAgain && noneOff && silent && messenger;
        });
        // copy() (hidden, as Android's system copies a channel), equals and hashCode; a channel through a Parcel (writeToParcel and
        // CREATOR, as Android's binder hands one over) and through the element of Android's notification policy file (writeXml and
        // populateFromXml, hidden: how NotificationManager keeps the app's channels across its runs) equal to its original; toString.
        step("notification-channel-copy", () -> {
            android.app.NotificationChannel c = new android.app.NotificationChannel("copied", "Copied", android.app.NotificationManager.IMPORTANCE_DEFAULT);
            c.setDescription("a channel to copy");
            c.setGroup("group");
            c.enableLights(true);
            c.setLightColor(0xFFFF0000);
            c.setVibrationPattern(new long[] { 100, 200, 300 });
            c.setShowBadge(false);
            c.setSound(android.provider.Settings.System.DEFAULT_NOTIFICATION_URI,
                    new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_NOTIFICATION_EVENT).build());
            android.app.NotificationChannel copy = (android.app.NotificationChannel) call(c, "copy", NO_TYPES);
            boolean copied = copy != c && c.equals(copy) && copy.equals(c) && c.hashCode() == copy.hashCode();
            copy.setImportance(android.app.NotificationManager.IMPORTANCE_LOW);
            boolean apart = !c.equals(copy) && c.getImportance() == android.app.NotificationManager.IMPORTANCE_DEFAULT;
            android.app.NotificationChannel other = new android.app.NotificationChannel("copied", "Copied", android.app.NotificationManager.IMPORTANCE_DEFAULT);
            boolean differs = !c.equals(other) && !other.equals(c);
            android.os.Parcel p = android.os.Parcel.obtain();
            c.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.app.NotificationChannel parcelled = android.app.NotificationChannel.CREATOR.createFromParcel(p);
            p.recycle();
            boolean throughParcel = parcelled != c && c.equals(parcelled) && c.hashCode() == parcelled.hashCode() && parcelled.getVibrationPattern() != c.getVibrationPattern();
            java.io.StringWriter xml = new java.io.StringWriter();
            org.xmlpull.v1.XmlSerializer out = android.util.Xml.newSerializer();
            out.setOutput(xml);
            out.startDocument("UTF-8", true);
            call(c, "writeXml", new Class<?>[] { org.xmlpull.v1.XmlSerializer.class }, out);
            out.endDocument();
            org.xmlpull.v1.XmlPullParser in = android.util.Xml.newPullParser();
            in.setInput(new java.io.StringReader(xml.toString()));
            android.app.NotificationChannel read = null;
            while (in.next() != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                if (in.getEventType() != org.xmlpull.v1.XmlPullParser.START_TAG || !"channel".equals(in.getName())) continue;
                read = new android.app.NotificationChannel(in.getAttributeValue(null, "id"), in.getAttributeValue(null, "name"),
                        Integer.parseInt(in.getAttributeValue(null, "importance")));
                call(read, "populateFromXml", new Class<?>[] { org.xmlpull.v1.XmlPullParser.class }, in);
            }
            boolean throughXml = read != null && c.equals(read) && read.equals(c);
            String said = c.toString();
            boolean told = said.startsWith("NotificationChannel{mId='copied', mName=Copied, mDescription=hasDescription , mImportance=3, ")
                    && said.contains(", mLightColor=-65536, mVibration=[100, 200, 300], ") && said.contains(", mShowBadge=false, ") && said.endsWith("}");
            detail = "copy " + copied + ", apart " + apart + ", another differs " + differs + ", through a parcel " + throughParcel + ", through XML " + throughXml
                     + " (" + xml + "), told " + said;
            return copied && apart && differs && throughParcel && throughXml && told;
        });
        // A channel group as Android's: its description kept, not blocked, one list of channels (empty until NotificationManager
        // gives the group out), clone() (public since Android 9: the stub had none), equals, through a Parcel and through XML.
        step("notification-channel-group", () -> {
            android.app.NotificationChannelGroup g = new android.app.NotificationChannelGroup("group", "Group");
            boolean fresh = "group".equals(g.getId()) && "Group".equals(g.getName().toString()) && g.getDescription() == null && !g.isBlocked()
                    && g.getChannels().isEmpty() && g.getChannels() == g.getChannels();
            g.setDescription("a group of channels");
            boolean described = "a group of channels".equals(g.getDescription());
            android.app.NotificationChannelGroup cloned = g.clone();
            boolean clones = cloned != g && cloned.equals(g) && g.equals(cloned) && cloned.hashCode() == g.hashCode();
            boolean differs = !g.equals(new android.app.NotificationChannelGroup("group", "Group"));
            android.os.Parcel p = android.os.Parcel.obtain();
            g.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.app.NotificationChannelGroup parcelled = android.app.NotificationChannelGroup.CREATOR.createFromParcel(p);
            p.recycle();
            boolean throughParcel = parcelled != g && parcelled.equals(g) && parcelled.hashCode() == g.hashCode();
            java.io.StringWriter xml = new java.io.StringWriter();
            org.xmlpull.v1.XmlSerializer out = android.util.Xml.newSerializer();
            out.setOutput(xml);
            out.startDocument("UTF-8", true);
            call(g, "writeXml", new Class<?>[] { org.xmlpull.v1.XmlSerializer.class }, out);
            out.endDocument();
            org.xmlpull.v1.XmlPullParser in = android.util.Xml.newPullParser();
            in.setInput(new java.io.StringReader(xml.toString()));
            android.app.NotificationChannelGroup read = null;
            while (in.next() != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                if (in.getEventType() != org.xmlpull.v1.XmlPullParser.START_TAG || !"channelGroup".equals(in.getName())) continue;
                read = new android.app.NotificationChannelGroup(in.getAttributeValue(null, "id"), in.getAttributeValue(null, "name"));
                call(read, "populateFromXml", new Class<?>[] { org.xmlpull.v1.XmlPullParser.class }, in);
            }
            boolean throughXml = read != null && read.equals(g);
            String said = g.toString();
            detail = "fresh " + fresh + ", described " + described + ", clone " + clones + ", another differs " + differs + ", through a parcel " + throughParcel
                     + ", through XML " + throughXml + " (" + xml + "), told " + said;
            return fresh && described && clones && differs && throughParcel && throughXml
                    && said.equals("NotificationChannelGroup{mId='group', mName=Group, mDescription=hasDescription , mBlocked=false, mChannels=[], mUserLockedFields=0}");
        });
    }

    /** The theme's android:colorForeground, 0 when it has none. */
    private static int foreground(android.content.res.Resources.Theme t) {
        android.util.TypedValue v = new android.util.TypedValue();
        return t.resolveAttribute(android.R.attr.colorForeground, v, true) ? v.data : 0;
    }

    /** Resources that answer one string themselves, as Tuya's dynamic strings answer theirs, and leave the rest to the original's. */
    static final class DynamicStrings extends android.content.res.Resources {
        @SuppressWarnings("deprecation")
        DynamicStrings(android.content.res.AssetManager a, android.util.DisplayMetrics m, android.content.res.Configuration c) {
            super(a, m, c);
        }

        @Override
        public CharSequence getText(int id) {
            return id == R.string.app_name ? "Dynamic" : super.getText(id);
        }
    }

    /** As the Settings library's MainSwitchBar: a LinearLayout that inflates its layout into itself and finds its views by id. */
    static final class SwitchBar extends android.widget.LinearLayout {
        final android.view.View frame;
        final android.widget.TextView title;
        final android.widget.Switch sw;

        SwitchBar(Context c) {
            super(c);
            android.view.LayoutInflater.from(c).inflate(R.layout.switch_bar, this);
            frame = findViewById(R.id.frame);
            title = (android.widget.TextView) findViewById(R.id.switch_text);
            sw = (android.widget.Switch) findViewById(android.R.id.switch_widget);
        }

        @Override
        public void setEnabled(boolean enabled) {
            super.setEnabled(enabled);
            title.setEnabled(enabled);
            sw.setEnabled(enabled);
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                frame.setEnabled(enabled);
                frame.setActivated(sw.isChecked());
            }
        }

        void setChecked(boolean checked) {
            sw.setChecked(checked);
            if (android.os.Build.VERSION.SDK_INT >= 31) frame.setActivated(checked);
        }
    }

    /**
     * A LocationRequest as Android 12 and later make one: the builder keeps the interval and the rest, the minimum update
     * interval a sixth of the interval until one is set, "any number" of updates (Integer.MAX_VALUE) until one is set; one of
     * the deprecated provider -- createFromDeprecatedProvider, as Google Play services' location engine asks for the passive
     * provider (hidden: by reflection) -- names its provider and the quality the passive provider stands for (POWER_NONE); one
     * goes through a Parcel and comes back equal. And a request of the passive interval through LocationManager (Android 12's
     * requestLocationUpdates with a LocationRequest, by reflection) is handed a fix on the main thread and removed:
     * "location-passive ok", which the test waits for. (LocationRequest is Android 12's: stubs/.)
     */
    private void locationRequest() {
        step("location-request", () -> {
            android.location.LocationRequest r = new android.location.LocationRequest.Builder(6000).setMaxUpdates(3).setMinUpdateDistanceMeters(10f).build();
            android.location.LocationRequest any = new android.location.LocationRequest.Builder(6000).build();
            Object deprecated = android.location.LocationRequest.class.getMethod("createFromDeprecatedProvider", String.class, long.class, float.class, boolean.class)
                    .invoke(null, "passive", 0L, 0f, false);
            String provider = (String) deprecated.getClass().getMethod("getProvider").invoke(deprecated);
            int quality = (Integer) deprecated.getClass().getMethod("getQuality").invoke(deprecated);
            android.os.Parcel p = android.os.Parcel.obtain();
            r.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.location.LocationRequest back = android.location.LocationRequest.CREATOR.createFromParcel(p);
            p.recycle();
            final android.location.LocationManager lm = (android.location.LocationManager) getSystemService(Context.LOCATION_SERVICE);
            android.location.LocationRequest passive = new android.location.LocationRequest.Builder(android.location.LocationRequest.PASSIVE_INTERVAL).build();
            lm.getClass().getMethod("requestLocationUpdates", String.class, android.location.LocationRequest.class, java.util.concurrent.Executor.class,
                    android.location.LocationListener.class).invoke(lm, android.location.LocationManager.PASSIVE_PROVIDER, passive, getMainExecutor(),
                    new android.location.LocationListener() {
                        private int calls;

                        @Override
                        public void onLocationChanged(android.location.Location l) {
                            if (++calls != 1) return;
                            boolean good = Math.abs(l.getLatitude() - 55.7512) < 0.01 && android.os.Looper.myLooper() == android.os.Looper.getMainLooper();
                            Log.i("DataTest", "location-passive " + (good ? "ok" : "FAILED " + l));
                            lm.removeUpdates(this);
                        }

                        @Override
                        public void onProviderEnabled(String provider) {}

                        @Override
                        public void onProviderDisabled(String provider) {}

                        @Override
                        public void onStatusChanged(String provider, int status, android.os.Bundle extras) {}
                    });
            detail = "built " + r + " minUpdate=" + r.getMinUpdateIntervalMillis() + " any.max=" + any.getMaxUpdates() + " deprecated " + deprecated + " provider=" + provider
                    + " quality=" + quality + " back=" + back;
            return r.getIntervalMillis() == 6000 && r.getMaxUpdates() == 3 && r.getMinUpdateIntervalMillis() == 1000
                    && r.getQuality() == android.location.LocationRequest.QUALITY_BALANCED_POWER_ACCURACY && any.getMaxUpdates() == Integer.MAX_VALUE
                    && "passive".equals(provider) && quality == 200 && r.equals(back);
        });
    }

    /**
     * Installing: an APK opened with ACTION_VIEW goes to Megler's installer (megler_system_host.install_apk), and so do a
     * PackageInstaller session's files when it is committed, which then sends its status, STATUS_SUCCESS, to the
     * PendingIntent it was given.
     */
    private void installs() {
        step("install-intent", () -> {
            java.io.File apk = new java.io.File(getCacheDir(), "Viewed.apk");
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(apk)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW)
                    .setDataAndType(android.net.Uri.fromFile(apk), "application/vnd.android.package-archive"));
            return getPackageManager().canRequestPackageInstalls();
        });
        step("install-session", () -> {
            android.content.pm.PackageInstaller pi = getPackageManager().getPackageInstaller();
            android.content.pm.PackageInstaller.SessionParams params =
                    new android.content.pm.PackageInstaller.SessionParams(android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName("com.example.store");
            int id = pi.createSession(params);
            // told of as Android tells of it, at once (Play Store asks before it writes)
            android.content.pm.PackageInstaller.SessionInfo info = pi.getSessionInfo(id);
            boolean mine = false;
            for (android.content.pm.PackageInstaller.SessionInfo i : pi.getMySessions()) if (i.getSessionId() == id) mine = true;
            if (info == null || info.getSessionId() != id || !"com.example.store".equals(info.getAppPackageName()) || !getPackageName().equals(info.getInstallerPackageName()) || !mine) {
                detail = "info=" + info + " mine=" + mine;
                return false;
            }
            android.content.pm.PackageInstaller.Session s = pi.openSession(id);
            try (java.io.OutputStream o = s.openWrite("base", 0, 4)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            // The names as written, as Android gives them (Play Store checks them against its artifacts), and the file read back by it.
            String[] names = s.getNames();
            byte[] back = new byte[4];
            int read = -1;
            try (java.io.InputStream in = s.openRead("base")) { if (in != null) read = in.read(back); }
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    Log.i("DataTest", "install-status " + i.getIntExtra("android.content.pm.extra.STATUS", -99) + " session=" + (i.getIntExtra("android.content.pm.extra.SESSION_ID", -1) == id)
                            + " package=" + i.getStringExtra("android.content.pm.extra.PACKAGE_NAME"));
                    // The app installed, uninstalled as an app store uninstalls one; and the other app, which the user keeps.
                    if (i.getIntExtra("android.content.pm.extra.STATUS", -99) == 0 && "com.example.split".equals(i.getStringExtra("android.content.pm.extra.PACKAGE_NAME"))) {
                        android.content.pm.PackageInstaller installer = getPackageManager().getPackageInstaller();
                        installer.uninstall("com.example.split", android.app.PendingIntent.getBroadcast(c, 2, new android.content.Intent("megler.test.UNINSTALL_STATUS"), 0).getIntentSender());
                        installer.uninstall("com.example.other", android.app.PendingIntent.getBroadcast(c, 3, new android.content.Intent("megler.test.UNINSTALL_STATUS"), 0).getIntentSender());
                    }
                }
            }, new android.content.IntentFilter("megler.test.INSTALL_STATUS"));
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    Log.i("DataTest", "uninstall-status " + i.getIntExtra("android.content.pm.extra.STATUS", -99) + " package=" + i.getStringExtra("android.content.pm.extra.PACKAGE_NAME"));
                }
            }, new android.content.IntentFilter("megler.test.UNINSTALL_STATUS"));
            android.app.PendingIntent status = android.app.PendingIntent.getBroadcast(this, 0, new android.content.Intent("megler.test.INSTALL_STATUS"),
                    0);
            s.commit(status.getIntentSender());
            detail = "id=" + id + " names=" + java.util.Arrays.toString(names) + " read=" + read;
            return id > 0 && names.length == 1 && "base".equals(names[0]) && read == 4 && back[0] == 'P' && back[3] == 4;
        });
        // A session of an app's base and a split, as Google Play installs an app built as a bundle: handed over as one .apks
        // file of both (the test opens it and counts them).
        step("install-splits", () -> {
            android.content.pm.PackageInstaller pi = getPackageManager().getPackageInstaller();
            android.content.pm.PackageInstaller.SessionParams params =
                    new android.content.pm.PackageInstaller.SessionParams(android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName("com.example.split");
            int id = pi.createSession(params);
            android.content.pm.PackageInstaller.Session s = pi.openSession(id);
            try (java.io.OutputStream o = s.openWrite("base.apk", 0, 4)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            try (java.io.OutputStream o = s.openWrite("split_config.xxhdpi.apk", 0, 4)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            // and ART's profile, as Google Play writes beside the APKs: kept, not handed over
            try (java.io.OutputStream o = s.openWrite("base.dm", 0, 4)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            String[] names = s.getNames();
            // As Android tells of packages that come and go: the test adds this one to the phone's apps, then takes it out.
            android.content.IntentFilter packages = new android.content.IntentFilter();
            for (String a : new String[] { android.content.Intent.ACTION_PACKAGE_ADDED, android.content.Intent.ACTION_PACKAGE_REMOVED,
                                           android.content.Intent.ACTION_PACKAGE_FULLY_REMOVED, android.content.Intent.ACTION_PACKAGE_REPLACED })
                packages.addAction(a);
            packages.addDataScheme("package");
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    Log.i("DataTest", "package-broadcast " + i.getAction() + " " + i.getData().getSchemeSpecificPart()
                            + " replacing=" + i.getBooleanExtra(android.content.Intent.EXTRA_REPLACING, false) + " uid=" + (i.getIntExtra(android.content.Intent.EXTRA_UID, -1) > 0));
                }
            }, packages);
            s.commit(android.app.PendingIntent.getBroadcast(this, 1, new android.content.Intent("megler.test.INSTALL_STATUS"), 0).getIntentSender());
            detail = "id=" + id + " names=" + java.util.Arrays.toString(names);
            return id > 0 && names.length == 3;
        });
    }

    /** As Material's BaseProgressIndicator: its indeterminate drawable is its own kind, cast from the ProgressBar's. */
    static final class Indicator extends android.widget.ProgressBar {
        static final class Own extends android.graphics.drawable.ColorDrawable {}
        Indicator(android.content.Context c) { super(c); }
        @Override public Own getIndeterminateDrawable() { return (Own) super.getIndeterminateDrawable(); }
    }

    /**
     * The phone's look as an app sees it (the test tells megler_set_theme a cobalt accent, #0063B1, and a light theme): the
     * platform's DeviceDefault theme gives it as colorAccent, Material You's system_accent1_600 (number 0x0106003f, tone 40
     * of the first accent palette, in HCT) is made of it, and the configuration is not night.
     */
    private void themeAccent() {
        step("theme-accent", () -> {
            android.content.res.TypedArray a = new android.view.ContextThemeWrapper(this, android.R.style.Theme_DeviceDefault_Light)
                    .obtainStyledAttributes(new int[] { android.R.attr.colorAccent });
            int accent = a.getColor(0, 0);
            a.recycle();
            int material = getResources().getColor(0x0106003f, null);
            int mode = getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK;
            detail = String.format("accent=%08x material=%08x night=%d", accent, material, mode);
            return accent == 0xFF0063B1 && Math.abs((material & 0xFF) - 0xAD) <= 1 && (material & 0xFFFFFF00) == 0xFF005F00 && mode == android.content.res.Configuration.UI_MODE_NIGHT_NO;
        });
    }

    /**
     * The platform's PlaybackState as PlaybackStateCompat makes it: through its Builder and, for an action of the app's own,
     * CustomAction.Builder; what was set is there, and through a Parcel it comes back by CREATOR.
     */
    private void playbackState() {
        step("playback-state", () -> {
            android.media.session.PlaybackState.CustomAction like = new android.media.session.PlaybackState.CustomAction.Builder("like", "Like",
                    android.R.drawable.star_on).build();
            android.media.session.PlaybackState s = new android.media.session.PlaybackState.Builder()
                    .setState(android.media.session.PlaybackState.STATE_PLAYING, 42000, 1.0f, 1234)
                    .setActions(android.media.session.PlaybackState.ACTION_PLAY_PAUSE | android.media.session.PlaybackState.ACTION_SEEK_TO)
                    .setBufferedPosition(60000).addCustomAction(like).build();
            android.os.Parcel p = android.os.Parcel.obtain();
            s.writeToParcel(p, 0);
            p.setDataPosition(0);
            android.media.session.PlaybackState back = android.media.session.PlaybackState.CREATOR.createFromParcel(p);
            p.recycle();
            detail = "back " + back;
            return back.getState() == android.media.session.PlaybackState.STATE_PLAYING && back.getPosition() == 42000
                    && back.getLastPositionUpdateTime() == 1234 && back.getPlaybackSpeed() == 1.0f && back.getBufferedPosition() == 60000
                    && back.getActions() == (android.media.session.PlaybackState.ACTION_PLAY_PAUSE | android.media.session.PlaybackState.ACTION_SEEK_TO)
                    && back.getCustomActions().size() == 1 && "like".equals(back.getCustomActions().get(0).getAction())
                    && "Like".contentEquals(back.getCustomActions().get(0).getName());
        });
    }

    /**
     * The platform's MediaSessionManager.RemoteUserInfo made by the app, as AndroidX Media makes one on API 28 and later when a
     * controller registers its callback (Deezer ended at its start with an InstantiationError: the class was an empty
     * abstract one): what it was made of comes back, and two of the same are equal.
     */
    /**
     * A key of the phone's media controls reaches the active session's callback with who sent it there to be asked
     * (getCurrentControllerInfo: the system, "android" with its uid), as Spotify asks it in onPause; outside of the callback
     * the question throws, as on Android. (MediaSession.meglerMediaKey is what the player calls for the key: by reflection.)
     */
    private void mediaKeyController() {
        step("media-key-controller", () -> {
            android.os.HandlerThread ht = new android.os.HandlerThread("media keys");
            ht.start();
            final android.media.session.MediaSession session = new android.media.session.MediaSession(this, "media-key-controller");
            final String[] seen = new String[1];
            final java.util.concurrent.CountDownLatch paused = new java.util.concurrent.CountDownLatch(1);
            session.setCallback(new android.media.session.MediaSession.Callback() {
                @Override public void onPause() {
                    try {
                        android.media.session.MediaSessionManager.RemoteUserInfo u = session.getCurrentControllerInfo();
                        seen[0] = u.getPackageName() + " uid " + u.getUid();
                    } catch (RuntimeException e) {
                        seen[0] = String.valueOf(e);
                    }
                    paused.countDown();
                }
            }, new android.os.Handler(ht.getLooper()));
            session.setPlaybackState(new android.media.session.PlaybackState.Builder()
                    .setState(android.media.session.PlaybackState.STATE_PLAYING, 0, 1f)
                    .setActions(android.media.session.PlaybackState.ACTION_PAUSE | android.media.session.PlaybackState.ACTION_PLAY).build());
            session.setActive(true);
            boolean threw = false;
            try { session.getCurrentControllerInfo(); } catch (IllegalStateException e) { threw = true; }
            boolean taken = (Boolean) android.media.session.MediaSession.class.getMethod("meglerMediaKey", android.view.KeyEvent.class)
                    .invoke(null, new android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_MEDIA_PAUSE));
            boolean called = paused.await(5, java.util.concurrent.TimeUnit.SECONDS);
            // (The key's message done whole before the question is asked again: one posted after it.)
            final java.util.concurrent.CountDownLatch drained = new java.util.concurrent.CountDownLatch(1);
            new android.os.Handler(ht.getLooper()).post(drained::countDown);
            drained.await(5, java.util.concurrent.TimeUnit.SECONDS);
            boolean threwAfter = false;
            try { session.getCurrentControllerInfo(); } catch (IllegalStateException e) { threwAfter = true; }
            session.release();
            ht.quit();
            detail = "outside " + threw + ", key taken " + taken + ", onPause " + called + " saw " + seen[0] + ", after " + threwAfter;
            return threw && taken && called && ("android uid " + android.os.Process.SYSTEM_UID).equals(seen[0]) && threwAfter;
        });
    }

    /**
     * A selector's constant state as Android's DrawableContainerState says it, as Spotify's SecondaryButtonView reads it: its
     * children, the very drawables it draws (getChildren, getChild, getChildCount); a copy of it (newDrawable) of the same
     * kind, with the same states and each child a copy of its own; a level list's and an animation's copies likewise.
     */
    private void containerState() {
        step("container-state", () -> {
            android.graphics.drawable.GradientDrawable pressed = new android.graphics.drawable.GradientDrawable();
            pressed.setColor(0xFFFF0000);
            android.graphics.drawable.GradientDrawable normal = new android.graphics.drawable.GradientDrawable();
            normal.setColor(0xFF00FF00);
            android.graphics.drawable.StateListDrawable s = new android.graphics.drawable.StateListDrawable();
            s.addState(new int[] { android.R.attr.state_pressed }, pressed);
            s.addState(new int[0], normal);
            android.graphics.drawable.Drawable.ConstantState cs = s.getConstantState();
            if (!(cs instanceof android.graphics.drawable.DrawableContainer.DrawableContainerState)) {
                detail = "the selector's constant state is " + cs;
                return false;
            }
            android.graphics.drawable.DrawableContainer.DrawableContainerState st = (android.graphics.drawable.DrawableContainer.DrawableContainerState) cs;
            android.graphics.drawable.Drawable[] children = st.getChildren();
            boolean live = children != null && children.length >= 2 && children[0] == pressed && children[1] == normal && st.getChildCount() == 2
                    && st.getChild(1) == normal && st.canConstantState();
            android.graphics.drawable.Drawable copy = cs.newDrawable();
            boolean copied = false;
            if (copy instanceof android.graphics.drawable.StateListDrawable && copy != s) {
                copy.setState(new int[] { android.R.attr.state_pressed });
                android.graphics.drawable.Drawable now = copy.getCurrent();
                copy.setState(new int[0]);
                android.graphics.drawable.Drawable rest = copy.getCurrent();
                copied = now instanceof android.graphics.drawable.GradientDrawable && now != pressed && rest instanceof android.graphics.drawable.GradientDrawable
                        && rest != normal && rest != now;
            }
            android.graphics.drawable.LevelListDrawable l = new android.graphics.drawable.LevelListDrawable();
            android.graphics.drawable.ColorDrawable low = new android.graphics.drawable.ColorDrawable(0xFF0000FF), high = new android.graphics.drawable.ColorDrawable(0xFFFFFF00);
            l.addLevel(0, 10, low);
            l.addLevel(11, 20, high);
            android.graphics.drawable.Drawable lc = l.getConstantState().newDrawable();
            lc.setLevel(15);
            boolean levels = lc instanceof android.graphics.drawable.LevelListDrawable && lc.getCurrent() instanceof android.graphics.drawable.ColorDrawable
                    && ((android.graphics.drawable.ColorDrawable) lc.getCurrent()).getColor() == 0xFFFFFF00 && lc.getCurrent() != high;
            android.graphics.drawable.AnimationDrawable a = new android.graphics.drawable.AnimationDrawable();
            a.addFrame(new android.graphics.drawable.ColorDrawable(0xFF000001), 100);
            a.addFrame(new android.graphics.drawable.ColorDrawable(0xFF000002), 200);
            a.setOneShot(true);
            android.graphics.drawable.Drawable ac = a.getConstantState().newDrawable();
            boolean frames = ac instanceof android.graphics.drawable.AnimationDrawable && ((android.graphics.drawable.AnimationDrawable) ac).getNumberOfFrames() == 2
                    && ((android.graphics.drawable.AnimationDrawable) ac).getDuration(1) == 200 && ((android.graphics.drawable.AnimationDrawable) ac).isOneShot();
            detail = "children " + (children == null ? "null" : children.length) + " live " + live + ", copy " + copy + " " + copied + ", levels " + levels + ", frames " + frames;
            return live && copied && levels && frames;
        });
    }

    /**
     * Android 12's Context.createContext(ContextParams), as Google Play services' Nearby UWB service asks it: a context of the
     * attribution tag the parameters carry, from this activity and from the application. (API 31, past the SDK the app is
     * built against: by reflection.)
     */
    private void contextParams() {
        step("context-params", () -> {
            Class<?> params = Class.forName("android.content.ContextParams"), builder = Class.forName("android.content.ContextParams$Builder");
            Object b = builder.getConstructor().newInstance();
            builder.getMethod("setAttributionTag", String.class).invoke(b, "megler-test");
            Object made = builder.getMethod("build").invoke(b);
            java.lang.reflect.Method create = android.content.Context.class.getMethod("createContext", params);
            Object fromActivity = create.invoke(this, made), fromApp = create.invoke(getApplicationContext(), made);
            detail = "from the activity " + fromActivity + ", from the application " + fromApp;
            return fromActivity instanceof android.content.Context && fromApp instanceof android.content.Context
                    && ((android.content.Context) fromApp).getPackageName().equals(getPackageName());
        });
    }

    private void remoteUserInfo() {
        step("remote-user-info", () -> {
            android.media.session.MediaSessionManager.RemoteUserInfo u = new android.media.session.MediaSessionManager.RemoteUserInfo("megler.test.data", 12, 10077);
            android.media.session.MediaSessionManager.RemoteUserInfo same = new android.media.session.MediaSessionManager.RemoteUserInfo("megler.test.data", 12, 10077);
            detail = u.getPackageName() + " " + u.getPid() + " " + u.getUid();
            return "megler.test.data".equals(u.getPackageName()) && u.getPid() == 12 && u.getUid() == 10077 && u.equals(same) && u.hashCode() == same.hashCode()
                    && !u.equals(new android.media.session.MediaSessionManager.RemoteUserInfo("megler.test.data", 13, 10077));
        });
    }

    /** A FrameLayout whose setWillNotDraw needs what its own constructor makes, as Chromium's CompositorView does. */
    private static final class WillNotDrawProbe extends android.widget.FrameLayout {
        private final Object made = new Object();
        int calls;
        WillNotDrawProbe(android.content.Context c) { super(c); }
        @Override public void setWillNotDraw(boolean w) { made.hashCode(); calls++; super.setWillNotDraw(w); }
    }

    /**
     * RecyclerView's ChildHelper in small: a child detached from the list into the scrap (detachViewFromParent, which leaves it
     * in the window, as Android's does), and a detached child removed (removeDetachedView). Both are protected in ViewGroup.
     */
    private static final class Scrapping extends android.widget.FrameLayout {
        Scrapping(android.content.Context c) { super(c); }
        void scrap(android.view.View v) { detachViewFromParent(v); }
        void removeScrapped(android.view.View v) { removeDetachedView(v, false); }
    }

    /**
     * A view animated by View.animate() goes on animating when it leaves the window, as on Android: its listener hears the
     * animation's end afterwards, not inside the removal. RecyclerView's layout takes a child from its scrap so (androidx's
     * Recycler.tryGetViewHolderForPositionByDeadline): found stale, the child is removed with removeDetachedView and then
     * unscrapped, which needs it still in the scrap; and the item animator's listener, when the child's animation ends,
     * recycles it (RecyclerView's removeAnimatingView: no longer a child, so unscrapped). Here the removal cancelled the
     * animation, the listener unscrapped the child inside it, and the unscrap that followed met null: Spotify's Now Playing
     * carousel, whose covers its item animator fades out through View.animate(), ended so at a track change (0.1.0.678). The
     * list does it once it is in the window, and says what was heard during the removal and, later, at the animation's end.
     */
    private void detachedAnimator() {
        step("detached-animator", () -> {
            final Scrapping list = new Scrapping(this);
            final android.view.View item = new android.view.View(this);
            item.setBackgroundColor(0xff663399);
            list.addView(item, new android.widget.FrameLayout.LayoutParams(20, 20));
            list.addOnAttachStateChangeListener(new android.view.View.OnAttachStateChangeListener() {
                @Override public void onViewAttachedToWindow(android.view.View v) {
                    list.removeOnAttachStateChangeListener(this);
                    list.post(() -> takeFromScrap(list, item));
                }
                @Override public void onViewDetachedFromWindow(android.view.View v) {}
            });
            ((android.view.ViewGroup) text.getParent()).addView(list, new android.view.ViewGroup.LayoutParams(40, 40));
            return true;
        });
    }

    private void takeFromScrap(final Scrapping list, final android.view.View item) {
        final Object[] scrapContainer = new Object[1];   /* the holder's mScrapContainer */
        final boolean[] removing = new boolean[1];
        final StringBuilder inside = new StringBuilder(), after = new StringBuilder();
        // The item animator's fade: cancelled or ended, the item is finished once (the listener taken off at the first, as
        // Spotify's is through ViewPropertyAnimatorCompat), and recycled as RecyclerView's removeAnimatingView recycles it.
        item.animate().alpha(0f).setDuration(100).setListener(new android.animation.AnimatorListenerAdapter() {
            private void finished(String how) {
                item.animate().setListener(null);
                (removing[0] ? inside : after).append(how).append(' ');
                if (list.indexOfChild(item) < 0) scrapContainer[0] = null;
                if (!removing[0]) list.post(() -> {
                    Log.i("DataTest", "detached-animator ended heard=" + after.toString().trim() + " alpha=" + item.getAlpha() + " attached=" + item.isAttachedToWindow());
                    ((android.view.ViewGroup) list.getParent()).removeView(list);
                });
            }
            @Override public void onAnimationCancel(android.animation.Animator a) { finished("cancel"); }
            @Override public void onAnimationEnd(android.animation.Animator a) { finished("end"); }
        }).start();
        boolean attached = item.isAttachedToWindow();
        // The layout: the child into the scrap, found stale and removed, then unscrapped (the holder's unScrap()).
        list.scrap(item);
        scrapContainer[0] = list;
        removing[0] = true;
        String unscrap;
        try {
            list.removeScrapped(item);
            unscrap = scrapContainer[0] != null ? "unscrapped" : "NullPointerException";
            scrapContainer[0] = null;
        } finally {
            removing[0] = false;
        }
        Log.i("DataTest", "detached-animator removed " + unscrap + " heard=" + (inside.length() == 0 ? "nothing" : inside.toString().trim())
              + " attached-before=" + attached + " after=" + item.isAttachedToWindow() + " parent=" + item.getParent());
    }

    /**
     * A ViewPropertyAnimator tells the listener it has when it tells, as Android's: taken off in onAnimationCancel, it does
     * not hear the onAnimationEnd that follows, and withEndAction's runnable is not run for an animation cancelled. A
     * property asked for again is taken from the animation that had it, which is cancelled then if it has none left -- its
     * listener hears that before a new one is set -- and an animation of other properties runs on beside the new one. And
     * start() with nothing asked for starts an animation of nothing, which its listener hears. Spotify's carousel animator
     * (through ViewPropertyAnimatorCompat) takes its listener off at a cancel to finish an item once; here an animation's
     * listener was the one set as it started, which heard the end too, and the item was finished twice.
     */
    private void animatorListener() {
        step("animator-listener", () -> {
            final android.view.View v = new android.view.View(this);
            final StringBuilder first = new StringBuilder(), taken = new StringBuilder(), next = new StringBuilder();
            final boolean[] endAction = new boolean[1];
            final int[] cancels = new int[1];
            v.animate().translationX(10).setDuration(5000).withEndAction(() -> endAction[0] = true).setListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationCancel(android.animation.Animator a) { first.append("cancel "); v.animate().setListener(null); }
                @Override public void onAnimationEnd(android.animation.Animator a) { first.append("end "); }
            }).start();
            v.animate().cancel();
            v.animate().alpha(0f).setListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationCancel(android.animation.Animator a) { taken.append("cancel "); }
                @Override public void onAnimationEnd(android.animation.Animator a) { taken.append("end "); }
            }).start();
            v.animate().alpha(1f).setListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationStart(android.animation.Animator a) { next.append("start "); }
                @Override public void onAnimationCancel(android.animation.Animator a) { cancels[0]++; }
            }).start();
            v.animate().scaleX(2f).start();
            int beside = cancels[0];
            v.animate().cancel();
            final StringBuilder empty = new StringBuilder();
            v.animate().setListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationStart(android.animation.Animator a) { empty.append("start "); }
                @Override public void onAnimationCancel(android.animation.Animator a) { empty.append("cancel "); }
                @Override public void onAnimationEnd(android.animation.Animator a) { empty.append("end "); }
            }).start();
            v.animate().cancel();
            detail = "first heard " + first.toString().trim() + ", its end action run " + endAction[0] + "; the one taken over heard " + taken.toString().trim()
                + "; the next " + next.toString().trim() + ", cancelled beside another " + beside + " times and by cancel() " + cancels[0]
                + "; one of nothing heard " + empty.toString().trim();
            return "cancel".equals(first.toString().trim()) && !endAction[0] && "cancel end".equals(taken.toString().trim())
                && "start start".equals(next.toString().trim()) && beside == 0 && cancels[0] == 2 && "start cancel end".equals(empty.toString().trim());
        });
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
                    o.write("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\nConnection: close\r\n\r\nmegler!".getBytes("ISO-8859-1"));
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

    // ---- RenderNodes and Pictures, as Compose uses them: display lists, drawn again with what the nodes have when drawn

    private android.graphics.Bitmap nodePic;
    private android.graphics.RenderNode[] kids;
    private android.graphics.Bitmap[] kidBitmaps;
    private android.graphics.RenderNode parent, grand, mid, top;
    private static final int NODES_W = 240, NODES_H = 240, KIDS = 12;

    /** A plain bitmap of that size. */
    private static android.graphics.Bitmap bitmap(int w, int h) {
        return android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
    }

    /** What a node draws: a background, an oval, a line, a label and a small bitmap, all inside (0, 0, w, h). Flat: the background alone. */
    private void content(android.graphics.Canvas c, int i, int w, int h, boolean flat) {
        android.graphics.Paint p = new android.graphics.Paint();
        p.setColor(0xFF000000 | ((i + 1) * 0x3B5A17 & 0xFFFFFF));
        c.drawRect(0, 0, w, h, p);
        if (flat) return;
        p.setColor(0xFFFFFFFF);
        c.drawOval(w / 4f, h / 4f, w * 3 / 4f, h * 3 / 4f, p);
        p.setColor(0xFF202020);
        p.setStrokeWidth(2);
        c.drawLine(1, h - 2, w - 1, h - 2, p);
        p.setTextSize(Math.min(12, h / 2f));
        c.drawText("n" + i, 2, h / 2f, p);
        c.drawBitmap(nodePic, w - 5, 1, null);
    }

    /** Kids 5, 6 and 7 draw flat: they are made translucent and scaled, where a layer and a scaled bitmap give the same pixels only for a picture of one colour. */
    private static boolean flat(int i) {
        return i >= 5 && i <= 7;
    }

    /**
     * A node drawn as the framework drew nodes before display lists, from a bitmap of its content: moved to its place,
     * through its matrix, at its alpha. What a list gives must be that, pixel for pixel, where both are the same picture.
     */
    private static void drawAsBitmap(android.graphics.Canvas c, android.graphics.RenderNode n, android.graphics.Bitmap made) {
        int save = c.save();
        c.translate(n.getLeft(), n.getTop());
        if (!n.hasIdentityMatrix()) {
            android.graphics.Matrix m = new android.graphics.Matrix();
            n.getMatrix(m);
            c.concat(m);
        }
        android.graphics.Paint p = null;
        if (n.getAlpha() < 1f) {
            p = new android.graphics.Paint();
            p.setAlpha(Math.round(n.getAlpha() * 255));
        }
        c.drawBitmap(made, 0, 0, p);
        c.restoreToCount(save);
    }

    /** How many pixels differ between two bitmaps of one size. */
    private static int differ(android.graphics.Bitmap a, android.graphics.Bitmap b) {
        int w = a.getWidth(), h = a.getHeight(), n = 0;
        int[] pa = new int[w * h], pb = new int[w * h];
        a.getPixels(pa, 0, w, 0, 0, w, h);
        b.getPixels(pb, 0, w, 0, 0, w, h);
        for (int i = 0; i < pa.length; i++) if (pa[i] != pb[i]) n++;
        return n;
    }

    private static int count(android.graphics.Bitmap a, int color) {
        int w = a.getWidth(), h = a.getHeight(), n = 0;
        int[] pa = new int[w * h];
        a.getPixels(pa, 0, w, 0, 0, w, h);
        for (int v : pa) if (v == color) n++;
        return n;
    }

    /** A node drawn on a bitmap's canvas, as the window's canvas draws Compose's top layer. */
    private static android.graphics.Bitmap drawn(android.graphics.RenderNode n, int w, int h) {
        android.graphics.Bitmap b = bitmap(w, h);
        android.graphics.Canvas c = new android.graphics.Canvas(b);
        c.drawRenderNode(n);
        return b;
    }

    /** The parent's picture as the kids' bitmaps give it, and the three screen-sized nodes' squares. */
    private android.graphics.Bitmap kidsAsBitmaps() {
        android.graphics.Bitmap want = bitmap(NODES_W, NODES_H);
        android.graphics.Canvas c = new android.graphics.Canvas(want);
        for (int i = 0; i < KIDS; i++) drawAsBitmap(c, kids[i], kidBitmaps[i]);
        android.graphics.Paint p = new android.graphics.Paint();
        for (int j = 0; j < 3; j++) {
            p.setColor(0xFF000000 | (0x40 << (8 * j)));
            c.drawRect(200, 10 + 20 * j, 206, 16 + 20 * j, p);
        }
        return want;
    }

    private void renderNodes() {
        /*
         * Twelve nodes and three the size of a phone's screen, recorded once, drawn by a parent recorded once: the picture is
         * what the framework's bitmap of each node gave, pixel for pixel, and a screen-sized node takes a few hundred bytes
         * (its bitmap was 14 MB).
         */
        step("render-nodes", () -> {
            nodePic = bitmap(4, 4);
            nodePic.eraseColor(0xFF00FFFF);
            nodePic.setPixel(0, 0, 0xFFFF00FF);
            kids = new android.graphics.RenderNode[KIDS];
            kidBitmaps = new android.graphics.Bitmap[KIDS];
            for (int i = 0; i < KIDS; i++) {
                int w = 20 + 7 * i + (i & 1), h = 16 + 5 * i, x = (i % 4) * 60, y = (i / 4) * 80;
                kids[i] = new android.graphics.RenderNode("kid" + i);
                kids[i].setPosition(x, y, x + w, y + h);
                if (i == 5) kids[i].setAlpha(0.5f);
                android.graphics.RecordingCanvas rc = kids[i].beginRecording(w, h);
                content(rc, i, w, h, flat(i));
                kids[i].endRecording();
                kidBitmaps[i] = bitmap(w, h);
                content(new android.graphics.Canvas(kidBitmaps[i]), i, w, h, flat(i));
            }
            android.graphics.RenderNode[] big = new android.graphics.RenderNode[3];
            for (int j = 0; j < 3; j++) {
                big[j] = new android.graphics.RenderNode("screen" + j);
                big[j].setPosition(0, 0, 1440, 2560);
                android.graphics.RecordingCanvas rc = big[j].beginRecording(1440, 2560);
                android.graphics.Paint p = new android.graphics.Paint();
                p.setColor(0xFF000000 | (0x40 << (8 * j)));
                rc.drawRect(200, 10 + 20 * j, 206, 16 + 20 * j, p);
                big[j].endRecording();
            }
            parent = new android.graphics.RenderNode("parent");
            parent.setPosition(0, 0, NODES_W, NODES_H);
            android.graphics.RecordingCanvas prc = parent.beginRecording(NODES_W, NODES_H);
            boolean hardware = prc.isHardwareAccelerated() && prc.getWidth() == NODES_W && prc.getHeight() == NODES_H;
            for (int i = 0; i < KIDS; i++) prc.drawRenderNode(kids[i]);
            for (int j = 0; j < 3; j++) prc.drawRenderNode(big[j]);
            parent.endRecording();
            android.graphics.Bitmap got = drawn(parent, NODES_W, NODES_H), want = kidsAsBitmaps();
            int d = differ(got, want), green = count(got, 0xFF004000);
            long bytes = big[0].computeApproximateMemoryUsage();
            detail = "differ=" + d + " screen-node-bytes=" + bytes + " hardware=" + hardware + " green=" + green;
            return d == 0 && green == 36 && hardware && parent.hasDisplayList() && bytes > 0 && bytes < 4096;
        });
        /* The kids' properties changed and nothing recorded again: the parent draws them as they are now, as their bitmaps drawn so give them. */
        step("render-node-props", () -> {
            android.graphics.Bitmap before = drawn(parent, NODES_W, NODES_H);
            kids[1].setTranslationX(13);
            kids[2].setTranslationY(-7);
            kids[4].setPosition(150, 150, 150 + kids[4].getWidth(), 150 + kids[4].getHeight());
            kids[5].setAlpha(0.75f);
            kids[6].setScaleX(2);
            kids[7].setTranslationX(-20);
            kids[7].setAlpha(0.25f);
            kids[9].setAlpha(0);
            android.graphics.Bitmap got = drawn(parent, NODES_W, NODES_H), want = kidsAsBitmaps();
            int d = differ(got, want), moved = differ(got, before);
            detail = "differ=" + d + " changed=" + moved;
            return d == 0 && moved > 1000;
        });
        /* A node in a node in a node, each moved by its own place; the innermost and the middle moved, nothing recorded again. */
        step("render-node-nested", () -> {
            android.graphics.Paint p = new android.graphics.Paint();
            grand = new android.graphics.RenderNode("grand");
            grand.setPosition(2, 3, 10, 9);
            android.graphics.RecordingCanvas rc = grand.beginRecording(8, 6);
            p.setColor(0xFF00FF00);
            rc.drawRect(0, 0, 8, 6, p);
            grand.endRecording();
            mid = new android.graphics.RenderNode("mid");
            mid.setPosition(20, 30, 60, 70);
            rc = mid.beginRecording(40, 40);
            p.setColor(0xFF333333);
            rc.drawRect(0, 0, 40, 40, p);
            rc.drawRenderNode(grand);
            mid.endRecording();
            top = new android.graphics.RenderNode("top");
            top.setPosition(0, 0, 100, 100);
            rc = top.beginRecording(100, 100);
            rc.drawRenderNode(mid);
            top.endRecording();
            android.graphics.Bitmap first = drawn(top, 100, 100);
            boolean firstAt = first.getPixel(23, 34) == 0xFF00FF00 && count(first, 0xFF00FF00) == 48;
            grand.setTranslationX(20);
            grand.setTranslationY(10);
            mid.setTranslationX(5);
            android.graphics.Bitmap got = drawn(top, 100, 100), want = bitmap(100, 100);
            android.graphics.Canvas c = new android.graphics.Canvas(want);
            p.setColor(0xFF333333);
            c.drawRect(25, 30, 65, 70, p);
            p.setColor(0xFF00FF00);
            c.drawRect(47, 43, 55, 49, p);
            int d = differ(got, want);
            detail = "first=" + firstAt + " differ=" + d;
            return firstAt && d == 0;
        });
        /* The innermost discarded: drawn as nothing; recorded again with something else: the outer lists, not recorded again, draw that. */
        step("render-node-rerecord", () -> {
            grand.discardDisplayList();
            android.graphics.Bitmap none = drawn(top, 100, 100);
            boolean gone = !grand.hasDisplayList() && count(none, 0xFF00FF00) == 0 && count(none, 0xFF333333) == 1600;
            android.graphics.RecordingCanvas rc = grand.beginRecording(8, 6);
            rc.drawColor(0xFFFF0000);
            grand.endRecording();
            android.graphics.Bitmap again = drawn(top, 100, 100);
            int red = count(again, 0xFFFF0000);
            detail = "gone=" + gone + " red=" + red + " at=" + Integer.toHexString(again.getPixel(50, 45));
            return gone && red == 48 && again.getPixel(50, 45) == 0xFFFF0000 && grand.hasDisplayList();
        });
        /*
         * What a list keeps: a paint, a path and a matrix as they were when recorded, whatever the app does to them afterwards;
         * a bitmap as it is when the list is drawn; a picture as it was when it was drawn into the list, whatever it records
         * afterwards.
         */
        step("render-node-snapshots", () -> {
            android.graphics.Paint p = new android.graphics.Paint();
            p.setColor(0xFFFF0000);
            android.graphics.Path path = new android.graphics.Path();
            path.addRect(0, 0, 10, 10, android.graphics.Path.Direction.CW);
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setTranslate(5, 5);
            android.graphics.Bitmap src = bitmap(4, 4);
            src.eraseColor(0xFF0000FF);
            android.graphics.Picture pic = new android.graphics.Picture();
            android.graphics.Canvas pc = pic.beginRecording(6, 6);
            android.graphics.Paint pp = new android.graphics.Paint();
            pp.setColor(0xFFFF00FF);
            pc.drawRect(0, 0, 6, 6, pp);
            pic.endRecording();
            android.graphics.RenderNode n = new android.graphics.RenderNode("snap");
            n.setPosition(0, 0, 40, 40);
            android.graphics.RecordingCanvas rc = n.beginRecording(40, 40);
            rc.save();
            rc.concat(m);
            rc.drawPath(path, p);
            rc.restore();
            rc.drawBitmap(src, 20, 20, null);
            rc.save();
            rc.translate(30, 30);
            rc.drawPicture(pic);
            rc.restore();
            n.endRecording();
            p.setColor(0xFF00FF00);
            path.reset();
            path.addRect(30, 0, 40, 10, android.graphics.Path.Direction.CW);
            m.setTranslate(0, 0);
            src.eraseColor(0xFFFFFF00);
            pc = pic.beginRecording(6, 6);
            pp.setColor(0xFF00FFFF);
            pc.drawRect(0, 0, 6, 6, pp);
            pic.endRecording();
            android.graphics.Bitmap got = drawn(n, 40, 40), alone = bitmap(6, 6);
            new android.graphics.Canvas(alone).drawPicture(pic);
            detail = "path=" + Integer.toHexString(got.getPixel(6, 6)) + " moved=" + Integer.toHexString(got.getPixel(31, 1)) + " bitmap="
                    + Integer.toHexString(got.getPixel(21, 21)) + " picture=" + Integer.toHexString(got.getPixel(32, 32)) + " picture-now="
                    + Integer.toHexString(alone.getPixel(3, 3)) + " red=" + count(got, 0xFFFF0000);
            return got.getPixel(6, 6) == 0xFFFF0000 && got.getPixel(14, 14) == 0xFFFF0000 && count(got, 0xFFFF0000) == 100 && got.getPixel(31, 1) == 0
                    && got.getPixel(21, 21) == 0xFFFFFF00 && got.getPixel(32, 32) == 0xFFFF00FF && alone.getPixel(3, 3) == 0xFF00FFFF;
        });
        /*
         * Two hundred nodes the size of a phone's screen, as a Compose screen of that many layers: recorded and drawn without
         * running out of memory. The framework's nodes took a bitmap each, 14 MB, and the app ended with an OutOfMemoryError.
         */
        step("render-node-memory", () -> {
            java.util.ArrayList<android.graphics.RenderNode> many = new java.util.ArrayList<>();
            android.graphics.Paint p = new android.graphics.Paint();
            long bytes = 0, started = System.nanoTime();
            String error = null;
            boolean shown = false;
            try {
                for (int i = 0; i < 200; i++) {
                    android.graphics.RenderNode n = new android.graphics.RenderNode("screen" + i);
                    n.setPosition(0, 0, 1440, 2560);
                    android.graphics.RecordingCanvas rc = n.beginRecording(1440, 2560);
                    rc.drawColor(0x08FFFFFF);
                    p.setColor(0xFF000000 | i);
                    rc.drawRect(i, 0, i + 1, 2560, p);
                    rc.drawText("layer " + i, 20, 40 + i, p);
                    n.endRecording();
                    many.add(n);
                    bytes += n.computeApproximateMemoryUsage();
                }
                android.graphics.RenderNode all = new android.graphics.RenderNode("all");
                all.setPosition(0, 0, 1440, 2560);
                android.graphics.RecordingCanvas rc = all.beginRecording(1440, 2560);
                for (android.graphics.RenderNode n : many) rc.drawRenderNode(n);
                all.endRecording();
                shown = drawn(all, 100, 100).getPixel(50, 50) != 0;
            } catch (OutOfMemoryError e) {
                error = e.toString();
            }
            long ms = (System.nanoTime() - started) / 1000000;
            detail = "nodes=" + many.size() + " bytes=" + bytes + " ms=" + ms + (error == null ? "" : " " + error);
            Log.i("DataTest", "render-node-memory nodes=" + many.size() + " bytes=" + bytes + " ms=" + ms + (error == null ? "" : " " + error));
            return error == null && many.size() == 200 && bytes < 200 * 2048 && shown;
        });
    }

    private static String describe(android.content.Intent battery) {
        return "level=" + battery.getIntExtra("level", -1) + "/" + battery.getIntExtra("scale", -1) + " status=" + battery.getIntExtra("status", -1) + " plugged="
                + battery.getIntExtra("plugged", -1) + " health=" + battery.getIntExtra("health", -1) + " voltage=" + battery.getIntExtra("voltage", -1) + " temperature="
                + battery.getIntExtra("temperature", -1) + " present=" + battery.getBooleanExtra("present", false) + " technology=" + battery.getStringExtra("technology");
    }

    /*
     * What the step "permissions" requests, and what it is to be told: the storage, the messages, the phone and the calls the
     * user lets the app have (the test's C side answers for the user); WRITE_EXTERNAL_STORAGE, asked for only up to Android 9,
     * denied with no one asked; the camera refused; VIBRATE, normal, granted; READ_CALENDAR, not in the manifest, denied;
     * READ_CONTACTS, refused for good in the app's App Permissions, denied with no one asked.
     */
    private static final String[] PERMISSIONS = { android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                                                  android.Manifest.permission.RECEIVE_SMS, android.Manifest.permission.READ_SMS, android.Manifest.permission.READ_PHONE_STATE,
                                                  android.Manifest.permission.READ_CALL_LOG, android.Manifest.permission.CAMERA, android.Manifest.permission.VIBRATE,
                                                  android.Manifest.permission.READ_CALENDAR, android.Manifest.permission.READ_CONTACTS };
    private static final int[] TOLD = { 0, -1, 0, 0, 0, 0, -1, 0, -1, -1 };
    private boolean cancelled, resumeAwaited, resumedNow;

    @Override
    protected void onPause() {
        super.onPause();
        resumedNow = false;
    }

    /*
     * The answers, as Android gives them: later, on the main thread, the activity paused around them where the user was asked
     * (Android's permission dialog is an activity over this one) and not where no one was. After the first no to the camera the
     * app may explain itself (the rationale) and ask again; a second no is for good, and a third request asks no one. Then a
     * fragment's request, answered to the fragment.
     */
    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        boolean main = android.os.Looper.myLooper() == android.os.Looper.getMainLooper(), paused = !resumedNow;
        String camera = android.Manifest.permission.CAMERA;
        boolean rationale = shouldShowRequestPermissionRationale(camera);
        if (request == 78) {
            cancelled = permissions.length == 0 && results.length == 0;
            return;
        }
        if (request == 77) {
            boolean all = main && Arrays.equals(permissions, PERMISSIONS) && Arrays.equals(results, TOLD);
            Log.i("DataTest", "permission-result " + (all ? "ok" : "FAILED " + Arrays.toString(results)) + " after done=" + doneLogged + " paused=" + paused
                    + " rationale=" + rationale);
            resumeAwaited = paused;
            requestPermissions(new String[] { camera }, rationale ? 79 : 80);
        } else if (request == 79) {
            Log.i("DataTest", "permission-again denied=" + (results.length == 1 && results[0] == -1) + " paused=" + paused + " rationale=" + rationale);
            requestPermissions(new String[] { camera }, 80);
        } else if (request == 80) {
            Log.i("DataTest", "permission-for-good denied=" + (results.length == 1 && results[0] == -1) + " paused=" + paused + " rationale=" + rationale);
            PermissionAsker asker = new PermissionAsker();
            getFragmentManager().beginTransaction().add(asker, "permissions").commitNow();
            asker.requestPermissions(new String[] { android.Manifest.permission.VIBRATE }, 81);
        } else {
            Log.i("DataTest", "permission-result FAILED: request " + request + " came to the activity");
        }
    }

    /* A fragment's request is answered to the fragment, not to its activity. */
    public static class PermissionAsker extends android.app.Fragment {
        @Override
        public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
            Log.i("DataTest", "permission-fragment " + (request == 81 && permissions.length == 1 && results.length == 1 && results[0] == 0 ? "ok" : "FAILED"));
        }
    }

    private boolean doneLogged, insetsLogged;

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DataTest", "onDestroy");
    }
}
