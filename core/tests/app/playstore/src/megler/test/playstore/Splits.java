package megler.test.playstore;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * The stand-in Play Store's split install service: ISplitInstallService's startInstall as the Play Store's own answers it
 * (com.google.android.finsky.splitinstallservice of Play Store 53.7, read for docs/PLAY-FEATURE-DELIVERY.md).
 *
 * On the calling thread it asks whose call this is -- the packages of Binder.getCallingUid must have the package the call
 * names, and a call that fails this is dropped without a word, as the Play Store drops it. The rest is on a thread of its
 * own: the app must be in front (getRunningAppProcesses), else the error ACCESS_DENIED; then the callback's
 * onStartInstall with the session, and the session's states as broadcasts to the app's package. The module `extra` is
 * installed as the Play Store installs one where the system takes splits: a PackageInstaller session that inherits the
 * app's APKs and holds the split. The module `files` is handed over as the Play Store does for SplitCompat: DOWNLOADED,
 * with the split named by a URI of this app's FileProvider. Any other module there is none of.
 */
public class Splits extends Service {
    private static final String T = Main.T;
    private static final String SERVICE = "com.google.android.play.core.splitinstall.protocol.ISplitInstallService";
    private static final String CALLBACK = "com.google.android.play.core.splitinstall.protocol.ISplitInstallServiceCallback";
    private static final String UPDATE = "com.google.android.play.core.splitinstall.receiver.SplitInstallUpdateIntentService";
    private static final String INSTALL_STATUS = "megler.test.playstore.INSTALL_STATUS";
    private static final int PENDING = 1, DOWNLOADING = 2, DOWNLOADED = 3, INSTALLING = 4, INSTALLED = 5, FAILED = 6;
    private static final int MODULE_UNAVAILABLE = -2, ACCESS_DENIED = -7, INTERNAL_ERROR = -100;

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final BlockingQueue<Intent> installed = new LinkedBlockingQueue<>();
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) { installed.add(intent); }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        registerReceiver(receiver, new IntentFilter(INSTALL_STATUS));
        Log.i(T, "service created");
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(receiver);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        Log.i(T, "service bound action=" + intent.getAction());
        return binder;
    }

    private final IBinder binder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != 2 /* startInstall */) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(SERVICE);
            final String pkg = data.readString();
            ArrayList<Bundle> list = data.createTypedArrayList(Bundle.CREATOR);
            Bundle options = data.readInt() != 0 ? Bundle.CREATOR.createFromParcel(data) : null;
            final IBinder callback = data.readStrongBinder();
            final ArrayList<String> modules = new ArrayList<>();
            if (list != null) for (Bundle b : list) if (b.getString("module_name") != null) modules.add(b.getString("module_name"));
            String[] owned = getPackageManager().getPackagesForUid(Binder.getCallingUid());
            boolean owns = owned != null && Arrays.asList(owned).contains(pkg);
            Log.i(T, "startInstall package=" + pkg + " modules=" + modules + " playcore=" + (options != null ? options.getInt("playcore_version_code") : 0)
                     + " caller-owns=" + owns);
            if (!owns) {
                Log.i(T, "refused: Package name " + pkg + " is not owned by caller.");
                return true;
            }
            worker.execute(() -> install(pkg, modules, callback));
            return true;
        }
    };

    /** Whether `pkg` has a process in front, as the Play Store asks before it downloads for an app. */
    private boolean inFront(String pkg) {
        List<ActivityManager.RunningAppProcessInfo> all = ((ActivityManager) getSystemService(ACTIVITY_SERVICE)).getRunningAppProcesses();
        if (all != null) for (ActivityManager.RunningAppProcessInfo p : all) {
            if (p.importance > ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) continue;
            if (pkg.equals(p.processName) || (p.pkgList != null && Arrays.asList(p.pkgList).contains(pkg))) return true;
        }
        return false;
    }

    private static Bundle state(int session, int status, int error, long done, long total, ArrayList<String> modules) {
        Bundle b = new Bundle();
        b.putInt("session_id", session);
        b.putInt("status", status);
        b.putInt("error_code", error);
        b.putLong("bytes_downloaded", done);
        b.putLong("total_bytes_to_download", total);
        b.putStringArrayList("module_names", new ArrayList<>(modules));
        b.putStringArrayList("languages", new ArrayList<String>());
        return b;
    }

    /** A session's state as the Play Store sends it: to the app's package alone, to the receivers it registered. */
    private void say(String pkg, Bundle state) {
        Log.i(T, "session " + state.getInt("session_id") + " state " + state.getInt("status") + " sent to " + pkg);
        sendBroadcast(new Intent(UPDATE).setPackage(pkg).putExtra("session_state", state).addFlags(0x40000000 /* FLAG_RECEIVER_REGISTERED_ONLY */));
    }

    private static void call(IBinder callback, int code, int session, Bundle bundle) {
        Parcel p = Parcel.obtain();
        try {
            p.writeInterfaceToken(CALLBACK);
            if (code == 2 /* onStartInstall */) p.writeInt(session);
            p.writeInt(1);
            bundle.writeToParcel(p, 0);
            callback.transact(code, p, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException e) {
            Log.e(T, "the callback's call " + code + " FAILED " + e);
        } finally {
            p.recycle();
        }
    }

    private static void error(IBinder callback, int code) {
        Bundle b = new Bundle();
        b.putInt("error_code", code);
        Log.i(T, "error " + code);
        call(callback, 6 /* onError */, 0, b);
    }

    private byte[] carried() throws java.io.IOException {
        try (InputStream in = getAssets().open("carried.apk")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    private void install(String pkg, ArrayList<String> modules, IBinder callback) {
        try {
            if (!inFront(pkg)) {
                Log.i(T, "Split install start download but in background: " + pkg);
                error(callback, ACCESS_DENIED);
                return;
            }
            String module = modules.size() == 1 ? modules.get(0) : "";
            if (!module.equals("extra") && !module.equals("files")) {
                error(callback, MODULE_UNAVAILABLE);
                return;
            }
            int session = module.equals("extra") ? 41 : 42;
            byte[] apk = carried();
            long total = apk.length;
            Log.i(T, "Start install for package: " + pkg + " session " + session);
            call(callback, 2 /* onStartInstall */, session, state(session, PENDING, 0, 0, total, modules));
            say(pkg, state(session, PENDING, 0, 0, total, modules));
            say(pkg, state(session, DOWNLOADING, 0, total / 2, total, modules));
            if (module.equals("files")) {
                /* For SplitCompat: the file where the app may read it, and the app copies it. (It is the same split under the
                   module's name: what the file says of itself is nothing the copying reads.) */
                try (FileOutputStream out = new FileOutputStream(new File(getCacheDir(), "files.apk"))) { out.write(apk); }
                ArrayList<Intent> files = new ArrayList<>();
                files.add(new Intent().setData(Uri.parse("content://" + Files.AUTHORITY + "/files.apk")).putExtra("module_name", "files").putExtra("split_id", "files")
                              .putExtra("version_code", 1).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
                Bundle s = state(session, DOWNLOADED, 0, total, total, modules);
                s.putParcelableArrayList("split_file_intents", files);
                say(pkg, s);
                return;
            }
            say(pkg, state(session, DOWNLOADED, 0, total, total, modules));
            say(pkg, state(session, INSTALLING, 0, total, total, modules));
            /* As the system's installer takes a feature module: a session that inherits what the app is installed with. */
            PackageInstaller pi = getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
            params.setAppPackageName(pkg);
            int id = pi.createSession(params);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (OutputStream o = s.openWrite(module, 0, apk.length)) {
                    o.write(apk);
                    s.fsync(o);
                }
                s.commit(PendingIntent.getBroadcast(this, id, new Intent(INSTALL_STATUS), 0).getIntentSender());
            }
            Intent result = installed.poll(60, TimeUnit.SECONDS);
            int status = result != null ? result.getIntExtra(PackageInstaller.EXTRA_STATUS, -99) : -99;
            Log.i(T, "install session " + id + " status=" + status + " package=" + (result != null ? result.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME) : null)
                     + " message=" + (result != null ? result.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) : null));
            say(pkg, status == PackageInstaller.STATUS_SUCCESS ? state(session, INSTALLED, 0, total, total, modules) : state(session, FAILED, INTERNAL_ERROR, total, total, modules));
        } catch (Exception e) {
            Log.e(T, "install FAILED", e);
            error(callback, INTERNAL_ERROR);
        }
    }
}
