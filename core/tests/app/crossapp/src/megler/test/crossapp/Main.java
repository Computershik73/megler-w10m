package megler.test.crossapp;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Parcelable;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import megler.test.shared.Note;
import megler.test.shared.Tag;

/**
 * One app working with another the phone has (the test app plugin), as Total Commander works with its plug-ins. A thread of
 * the app's goes through the steps and says how each went; what the other app got of them the test sees in that app's own
 * run, which follows this one (core/tests/mg_java_tests.c: run_crossapp, run_plugin).
 */
public class Main extends Activity {
    private static final String T = "CrossAppTest";
    private static final String PLUGIN = "megler.test.plugin";
    /* Where the intents for the other app wait for it, in the folder every app has (megler.PhoneApps). */
    private static final File INTENTS = new File("/data/megler-services/intents/" + PLUGIN);
    private int ok, failed;
    private String detail = "";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Cross-app");
        setContentView(v);
        new Thread(this::steps, "steps").start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }

    private interface Step { boolean run() throws Exception; }

    private void step(String name, Step s) {
        boolean good;
        detail = "";
        try {
            good = s.run();
        } catch (Throwable e) {
            Log.e(T, name + " threw", e);
            good = false;
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED " + detail));
    }

    /* On the main thread, as an app's screen does it; false when it threw or did not run within a minute. */
    private boolean onMain(Runnable r) throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        boolean[] good = { false };
        runOnUiThread(() -> {
            try {
                r.run();
                good[0] = true;
            } catch (Throwable e) {
                Log.e(T, "on the main thread", e);
                detail += " threw " + e;
            }
            done.countDown();
        });
        return done.await(60, TimeUnit.SECONDS) && good[0];
    }

    private static int waiting() {
        String[] names = INTENTS.list();
        return names == null ? 0 : names.length;
    }

    private static Uri uri(String path) { return Uri.parse("content://megler.test.crossapp/" + path); }

    private void steps() {
        PackageManager pm = getPackageManager();
        /* The other app is installed: its package's version and label, and its launcher's intent. */
        step("installed", () -> {
            PackageInfo p = pm.getPackageInfo(PLUGIN, 0);
            ApplicationInfo a = pm.getApplicationInfo(PLUGIN, 0);
            Intent launch = pm.getLaunchIntentForPackage(PLUGIN);
            detail = "version=" + p.versionName + "/" + p.versionCode + " label=" + pm.getApplicationLabel(a) + " launch=" + launch;
            return "1.7".equals(p.versionName) && p.versionCode == 7 && "Megler Plug-in".contentEquals(pm.getApplicationLabel(a)) && launch != null
                && (PLUGIN + ".Main").equals(launch.getComponent().getClassName());
        });
        /* An intent that names the package and what to do resolves to the activity whose filter takes it, and to none that takes it not. */
        step("resolve", () -> {
            Intent show = new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "text/plain");
            ResolveInfo r = pm.resolveActivity(show, 0);
            ResolveInfo none = pm.resolveActivity(new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "image/png"), 0);
            detail = "resolved=" + (r == null ? null : r.activityInfo.packageName + "/" + r.activityInfo.name) + " none=" + none;
            return r != null && PLUGIN.equals(r.activityInfo.packageName) && (PLUGIN + ".Shown").equals(r.activityInfo.name) && none == null;
        });
        /* Such an intent started: the other app is asked for that activity, and the intent waits for it, written out. */
        step("send-implicit", () -> {
            int before = waiting();
            Intent show = new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "text/plain").putExtra("text", "first");
            boolean sent = onMain(() -> startActivity(show));
            detail += " sent=" + sent + " waiting=" + before + "->" + waiting();
            return sent && before == 0 && waiting() == 1;
        });
        /*
         * The other app's activity by its class, for a result, with an Intent of everything: an action, data with a type, two
         * categories, flags, and extras -- strings, numbers, arrays, a Bundle, a list of Uris (what Total Commander's Wi-Fi
         * plug-in gets the files to send in), a Parcelable and a Serializable of classes both apps have, an Intent. A
         * Parcelable this app alone has goes and is left out where it is read; a Binder does not go at all.
         */
        step("send", () -> {
            Intent i = new Intent(PLUGIN + ".SHOW").setClassName(PLUGIN, PLUGIN + ".Shown").setDataAndType(uri("notes/7?x=1#part"), "text/plain")
                .addCategory(Intent.CATEGORY_DEFAULT).addCategory("megler.test.category.EXTRA").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            i.putExtra("text", "hello, plug-in").putExtra("wide", "é中😀").putExtra("count", 42).putExtra("big", 1234567890123L)
                .putExtra("flag", true).putExtra("ratio", 2.5).putExtra("longs", new long[] { 1, -2, 3000000000L }).putExtra("bytes", new byte[] { 1, -2, 127 })
                .putExtra("strings", new String[] { "a", null, "c" });
            Bundle nested = new Bundle();
            nested.putString("inner", "in");
            nested.putInt("n", 5);
            nested.putStringArrayList("list", new ArrayList<>(Arrays.asList("x", "y")));
            i.putExtra("nested", nested);
            ArrayList<Uri> uris = new ArrayList<>(Arrays.asList(uri("files/a.txt"), uri("files/b%20c.txt")));
            i.putParcelableArrayListExtra("uris", uris);
            i.putExtra(Intent.EXTRA_STREAM, uris.get(0));
            Bundle more = new Bundle();
            more.putString("k", "v");
            i.putExtra("note", new Note(7, "seven", uri("notes/7"), more, new Note(8, "eight", null, null, null), "t1", "t2"));
            i.putExtra("notes", new Parcelable[] { new Note(1, "one", null, null, null), new Note(2, "two", null, null, null) });
            i.putExtra("tag", new Tag("blue", 3));
            i.putExtra("only", new Only(5));
            i.putExtra("inner", new Intent("megler.test.INNER").putExtra("deep", "down"));
            Bundle binder = new Bundle();
            binder.putBinder("binder", new Binder());
            i.putExtras(binder);
            boolean sent = onMain(() -> startActivityForResult(i, 77));
            String[] names = INTENTS.list();
            detail += " sent=" + sent + " waiting=" + Arrays.toString(names);
            if (names == null || names.length != 2) return false;
            for (String name : names) if (!name.matches("[0-9a-f-]+")) return false;
            return sent;
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }
}
