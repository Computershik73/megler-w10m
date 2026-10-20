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
        connectivity();
        dnsServers();
        systemLibs();
        apacheHttpLegacy();
        preloadedDrawables();
        location();
        timezone();
        locale();
        phoneNumber();
        tls();
        keystore(run);
        codecs();
        userHandle();
        cleartext();
        compatChange();
        sharedLibrary();
        cameras();
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
        step("package-installer", () -> {
            android.content.pm.PackageInstaller installer = getPackageManager().getPackageInstaller();
            return installer != null && installer == getPackageManager().getPackageInstaller()
                    && installer.getMySessions() != null && installer.getMySessions().isEmpty();
        });
        wifi();
        mediaMetadata();
        playbackState();
        themeAccent();
        phoneApps();
        installs();
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
     */
    private void wifi() {
        step("wifi", () -> {
            android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            android.net.wifi.WifiInfo info = wm.getConnectionInfo();
            android.net.DhcpInfo dhcp = wm.getDhcpInfo();
            android.net.NetworkInfo net = cm.getNetworkInfo(android.net.ConnectivityManager.TYPE_WIFI);
            detail = "enabled=" + wm.isWifiEnabled() + " state=" + wm.getWifiState() + " info=" + (info == null ? null : info.getSSID() + "/" + info.getSupplicantState()
                    + "/" + info.getRssi() + "/" + info.getIpAddress()) + " dhcp=" + (dhcp == null ? null : dhcp.ipAddress) + " level=" + android.net.wifi.WifiManager.calculateSignalLevel(-50, 5);
            return wm.isWifiEnabled() && wm.getWifiState() == android.net.wifi.WifiManager.WIFI_STATE_ENABLED && net != null && net.isConnected()
                    && info != null && "<unknown ssid>".equals(info.getSSID()) && info.getSupplicantState() == android.net.wifi.SupplicantState.COMPLETED
                    && "02:00:00:00:00:00".equals(info.getBSSID()) && info.getRssi() == -50 && info.getLinkSpeed() > 0 && dhcp != null && dhcp.ipAddress == info.getIpAddress()
                    && android.net.wifi.WifiManager.calculateSignalLevel(-50, 5) == 4 && android.net.wifi.WifiManager.calculateSignalLevel(-100, 5) == 0
                    && android.net.wifi.WifiManager.calculateSignalLevel(-80, 5) == 1;
        });
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
            }, 400);
        }, 300);
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
     * Cleartext HTTP, as Android's network security policy decides it for this app: written for Android 9 and saying nothing in
     * its manifest (no usesCleartextTraffic, no network security configuration), it is refused, for any host. The WebView
     * asks this of every http:// request; the framework refused it to every app.
     */
    private void cleartext() {
        step("cleartext", () -> {
            android.security.NetworkSecurityPolicy p = android.security.NetworkSecurityPolicy.getInstance();
            return !p.isCleartextTrafficPermitted() && !p.isCleartextTrafficPermitted("example.com") && p == android.security.NetworkSecurityPolicy.getInstance();
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
                    boolean held = relayHeard == 1 && targetHeard == 1 && offHeard == 0 && registered[0] == 1;
                    Log.i("DataTest", "broadcast-relay " + (held ? "ok" : "FAILED") + " relay=" + relayHeard + " target=" + targetHeard + " off=" + offHeard + " registered=" + registered[0]);
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
     * Sound from Java, which megler.Audio plays: a playing AudioTrack takes a second of PCM in about a second, as its blocking
     * write does; a MediaPlayer plays a WAV the step writes and an Ogg Vorbis asset (tone.ogg: 440 Hz, half a second, 22050 Hz
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
            return written == rate && ms >= 600 && head > 0;
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
                if ("com.example.other".equals(r.activityInfo.packageName))
                    listed = "Other".equals(String.valueOf(r.loadLabel(pm))) && r.loadIcon(pm) != null && "com.example.other.Main".equals(r.activityInfo.name);
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
            android.content.pm.PackageInstaller.Session s = pi.openSession(id);
            try (java.io.OutputStream o = s.openWrite("base", 0, 4)) { o.write(new byte[] { 'P', 'K', 3, 4 }); }
            String[] names = s.getNames();
            registerReceiver(new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    Log.i("DataTest", "install-status " + i.getIntExtra("android.content.pm.extra.STATUS", -99) + " session=" + (i.getIntExtra("android.content.pm.extra.SESSION_ID", -1) == id)
                            + " package=" + i.getStringExtra("android.content.pm.extra.PACKAGE_NAME"));
                }
            }, new android.content.IntentFilter("megler.test.INSTALL_STATUS"));
            android.app.PendingIntent status = android.app.PendingIntent.getBroadcast(this, 0, new android.content.Intent("megler.test.INSTALL_STATUS"),
                    0);
            s.commit(status.getIntentSender());
            detail = "id=" + id + " names=" + java.util.Arrays.toString(names);
            return id > 0 && names.length == 1 && "base.apk".equals(names[0]);
        });
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
