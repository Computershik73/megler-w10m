package megler.test.playclient;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetFileDescriptor;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * An app's side of Play Feature Delivery, as Play Core 1.10.x (playcore_version_code 11004) speaks it: the service of
 * com.android.vending bound under BIND_SPLIT_INSTALL_SERVICE, one-way transactions of ISplitInstallService with a
 * callback binder, and the session states the Play Store broadcasts to the app's package.
 *
 * What it does depends on what it finds, as the test runs it several times over one data folder with the stand-in for the
 * Play Store run in between (core/tests/mg_java_tests.c, run_playclient): without the module `extra` it asks for it and
 * says what it was told; with it, it loads the module's class, asks again, asks the service's other questions and then for
 * the module `files`; and on the run after that it takes that module's file as SplitCompat takes one. A thread of the
 * app's goes through the steps; the callback's calls and the broadcasts come into a queue the steps read.
 */
public class Main extends Activity {
    private static final String T = "PlayClientTest";
    private static final String ACTION = "com.google.android.play.core.splitinstall.BIND_SPLIT_INSTALL_SERVICE";
    private static final String SERVICE = "com.google.android.play.core.splitinstall.protocol.ISplitInstallService";
    private static final String CALLBACK = "com.google.android.play.core.splitinstall.protocol.ISplitInstallServiceCallback";
    private static final String UPDATE = "com.google.android.play.core.splitinstall.receiver.SplitInstallUpdateIntentService";
    private static final int START_INSTALL = 2, CANCEL_INSTALL = 4, GET_SESSION_STATES = 6, DEFERRED_INSTALL = 8;

    private int ok, failed;
    private String detail = "";
    private volatile IBinder service;
    private volatile Bundle lastState;
    /* What was heard, in order: "connected <package>", "callback <call> session=<n>", "error <code>", "sessions <count>",
       "state <status> session=<n>[ error=<code>][ files=<count>]". */
    private final BlockingQueue<String> heard = new LinkedBlockingQueue<>();

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            Bundle s = intent.getBundleExtra("session_state");
            if (s == null) {
                heard.add("state none");
                return;
            }
            lastState = s;
            ArrayList<Intent> files = s.getParcelableArrayList("split_file_intents");
            heard.add("state " + s.getInt("status") + " session=" + s.getInt("session_id") + (s.getInt("error_code") != 0 ? " error=" + s.getInt("error_code") : "")
                      + (files != null ? " files=" + files.size() : ""));
        }
    };

    private final IBinder callback = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code < 2 || code > 13) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(CALLBACK);
            if (code == 7 /* onGetSessionStates */) {
                ArrayList<Bundle> list = data.createTypedArrayList(Bundle.CREATOR);
                heard.add("sessions " + (list != null ? list.size() : -1));
                return true;
            }
            int session = code <= 5 ? data.readInt() : -1;
            Bundle b = data.readInt() != 0 ? Bundle.CREATOR.createFromParcel(data) : null;
            if (code == 6 /* onError */) heard.add("error " + (b != null ? b.getInt("error_code") : 0));
            else heard.add("callback " + code + (code <= 5 ? " session=" + session : ""));
            return true;
        }
    };

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder b) {
            service = b;
            heard.add("connected " + name.getPackageName());
        }
        @Override public void onServiceDisconnected(ComponentName name) { heard.add("disconnected"); }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Play client");
        setContentView(v);
        registerReceiver(receiver, new IntentFilter(UPDATE));
        new Thread(this::steps, "steps").start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        unregisterReceiver(receiver);
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

    /** What is heard until something that begins with `last`, within `seconds`: all of it, joined with ";". */
    private String until(String last, int seconds) throws InterruptedException {
        long until = SystemClock.elapsedRealtime() + seconds * 1000L;
        StringBuilder all = new StringBuilder();
        for (;;) {
            long left = until - SystemClock.elapsedRealtime();
            String s = left > 0 ? heard.poll(left, TimeUnit.MILLISECONDS) : null;
            if (s == null) return all + (all.length() > 0 ? ";" : "") + "(nothing like \"" + last + "\")";
            all.append(all.length() > 0 ? ";" : "").append(s);
            if (s.startsWith(last)) return all.toString();
        }
    }

    /** A transaction of the service's as Play Core's proxy writes it: the package, what the call is about, the callback. */
    private void ask(int code, String module, int session) throws RemoteException {
        Parcel p = Parcel.obtain();
        try {
            p.writeInterfaceToken(SERVICE);
            p.writeString(getPackageName());
            if (code == START_INSTALL || code == DEFERRED_INSTALL) {
                ArrayList<Bundle> list = new ArrayList<>();
                Bundle m = new Bundle();
                m.putString("module_name", module);
                list.add(m);
                p.writeTypedList(list);
                Bundle options = new Bundle();
                options.putInt("playcore_version_code", 11004);
                p.writeInt(1);
                options.writeToParcel(p, 0);
            } else if (code == CANCEL_INSTALL) {
                p.writeInt(session);
            }
            p.writeStrongBinder(callback);
            service.transact(code, p, null, IBinder.FLAG_ONEWAY);
        } finally {
            p.recycle();
        }
    }

    private boolean has(String split) {
        String[] names = getApplicationInfo().splitNames;
        return names != null && Arrays.asList(names).contains(split);
    }

    private void steps() {
        final File marker = new File(getFilesDir(), "files-asked");
        final boolean extra = has("extra");
        Log.i(T, "splits " + Arrays.toString(getApplicationInfo().splitNames) + " files-asked=" + marker.exists());
        if (extra) {
            /* The module the Play Store added is among the app's splits, as Android tells an app of them. */
            step("split-listed", () -> {
                ApplicationInfo a = getPackageManager().getApplicationInfo(getPackageName(), 0);
                String[] names = getPackageManager().getPackageInfo(getPackageName(), 0).splitNames;
                detail = Arrays.toString(a.splitNames) + " " + Arrays.toString(a.splitSourceDirs) + " " + Arrays.toString(names);
                if (a.splitNames == null || a.splitSourceDirs == null || a.splitNames.length != 1 || a.splitSourceDirs.length != 1 || !"extra".equals(a.splitNames[0])) return false;
                /* Its file beside the base, under the name Android installs a split as. */
                File f = new File(a.splitSourceDirs[0]);
                return f.getName().equals("split_extra.apk") && new File(a.sourceDir).getParent().equals(f.getParent()) && f.isFile()
                    && names != null && Arrays.asList(names).contains("extra");
            });
            /* And its code is the app's: of the app's own class loader. */
            step("split-loaded", () -> {
                Class<?> c = Class.forName("megler.test.playclient.extra.Extra");
                Object word = c.getMethod("word").invoke(null);
                detail = c.getClassLoader() + " " + word;
                return c.getClassLoader() == getClassLoader() && "extra".equals(word);
            });
        }
        /* The bind Play Core makes: the Play Store's package under the service's action. */
        step("bound", () -> {
            boolean bound = bindService(new Intent(ACTION).setPackage("com.android.vending"), connection, Context.BIND_AUTO_CREATE);
            String got = until("connected", 30);
            detail = "bound=" + bound + " " + got;
            return bound && got.equals("connected com.android.vending") && service != null;
        });
        if (!extra) {
            /*
             * The module asked for. Nothing can answer in this run but Megler: at once with PLAY_STORE_NOT_FOUND where the
             * phone has no Play Store; and where it has one that does not take the request up -- the test's shell starts
             * none -- with SERVICE_DIED, some twenty seconds on.
             */
            long asked = SystemClock.elapsedRealtime();
            String got = "";
            try {
                ask(START_INSTALL, "extra", 0);
                got = until("error", 90);
            } catch (Exception e) {
                Log.e(T, "startInstall threw", e);
            }
            final long took = SystemClock.elapsedRealtime() - asked;
            final String told = got;
            Log.i(T, "asked extra: " + told + " quick=" + (took < 10000));
            if (told.equals("error -14")) step("no-store", () -> { detail = told + " after " + took + " ms"; return took < 10000; });
            else step("unanswered", () -> { detail = told + " after " + took + " ms"; return told.equals("error -9") && took >= 15000 && took < 60000; });
        } else if (!marker.exists()) {
            /* Asked again, as an app asks at its next start: answered from what the Play Store said, every call and state in its order. */
            step("replayed", () -> {
                ask(START_INSTALL, "extra", 0);
                String got = until("state 5", 60);
                detail = got;
                return got.equals("callback 2 session=41;state 1 session=41;state 2 session=41;state 3 session=41;state 4 session=41;state 5 session=41");
            });
            /* The app's sessions: none, the request ended. */
            step("session-states", () -> {
                ask(GET_SESSION_STATES, null, 0);
                String got = until("sessions", 30);
                detail = got;
                return got.equals("sessions 0");
            });
            /* A deferred install: the part of the API that is not here, and says so. */
            step("deferred", () -> {
                ask(DEFERRED_INSTALL, "extra", 0);
                String got = until("error", 30);
                detail = got;
                return got.equals("error -5");
            });
            /* A session to cancel that there is none of. */
            step("cancel-unknown", () -> {
                ask(CANCEL_INSTALL, null, 77);
                String got = until("error", 30);
                detail = got;
                return got.equals("error -4");
            });
            /* Another module asked for: a new request, left for the Play Store's next run. */
            step("files-asked", () -> {
                try (FileOutputStream out = new FileOutputStream(marker)) { out.write('1'); }
                ask(START_INSTALL, "files", 0);
                return marker.exists();
            });
        } else {
            /*
             * The module the Play Store hands over as files: asked again, the answer is what it said, DOWNLOADED with the
             * split's file -- where this app can read it -- which is copied as SplitCompat copies one, and loaded.
             */
            final File[] copy = new File[1];
            step("files-given", () -> {
                ask(START_INSTALL, "files", 0);
                String got = until("state 3", 60);
                detail = got;
                Bundle s = lastState;
                ArrayList<Intent> files = s != null ? s.<Intent>getParcelableArrayList("split_file_intents") : null;
                if (!got.equals("callback 2 session=42;state 1 session=42;state 2 session=42;state 3 session=42 files=1") || files == null || files.size() != 1) return false;
                Intent f = files.get(0);
                detail += " " + f.getData() + " " + f.getStringExtra("module_name") + " " + f.getStringExtra("split_id") + " " + f.getIntExtra("version_code", -1);
                File dir = new File(getFilesDir(), "splitcompat/1/unverified-splits");
                dir.mkdirs();
                File to = new File(dir, f.getStringExtra("split_id") + ".apk");
                long n = 0;
                try (AssetFileDescriptor fd = getContentResolver().openAssetFileDescriptor(f.getData(), "r"); InputStream in = fd.createInputStream();
                     FileOutputStream out = new FileOutputStream(to)) {
                    byte[] buf = new byte[4096];
                    for (int r; (r = in.read(buf)) > 0; n += r) out.write(buf, 0, r);
                }
                detail += " copied=" + n;
                copy[0] = to;
                return "files".equals(f.getStringExtra("module_name")) && "files".equals(f.getStringExtra("split_id")) && f.getIntExtra("version_code", -1) == 1
                    && n > 0 && to.length() == n;
            });
            step("files-loaded", () -> {
                if (copy[0] == null) return false;
                ClassLoader loader = new dalvik.system.PathClassLoader(copy[0].getPath(), String.class.getClassLoader());
                Class<?> c = loader.loadClass("megler.test.playclient.extra.Extra");
                Object word = c.getMethod("word").invoke(null);
                detail = c.getClassLoader() + " " + word;
                return c.getClassLoader() == loader && "extra".equals(word);
            });
        }
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }
}
