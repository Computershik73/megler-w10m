// Megler: an APK an app wants installed, handed to Megler's installer (Native.installApk, megler.h's
// megler_system_host.install_apk), which asks the user and installs it as an app of the phone's own -- as Android hands
// it to its package installer.
//
// Two ways in, as on Android. An intent: ACTION_INSTALL_PACKAGE, or ACTION_VIEW of an APK (its type
// application/vnd.android.package-archive, or a name ending .apk, .apks or .xapk) with no activity of its own named --
// what a file manager, a browser's download or an app store opens; a file:// URI is the file itself, a content:// one (an
// androidx FileProvider's) is copied out of its provider into the app's cache first. And a PackageInstaller session
// (android.content.pm.PackageInstaller): the files written into it, committed; one file is that APK, several are one app's
// base and splits -- what Google Play installs of an app built as a bundle -- handed over as one .apks file of them, which
// Megler's installer unpacks as it does a bundle from the file manager (the splits for this phone's screen, languages and
// processor). The files go by the app's package name (<package>.apk, <package>.apks), not by the names the app wrote them
// under: Google Play's are "stream..<package>", which Megler's link refuses for its "..". The dex metadata Google Play
// writes beside an app's APKs (a .dm file: ART's profile) is kept in the session and not handed over: Megler's installer
// takes APKs.
//
// A committed session is told its outcome through its IntentSender as Android tells it, once there is one: STATUS_SUCCESS
// when the phone's apps (PhoneApps) have the app at the APK's version -- Google Play looks for that version as soon as it
// is told, and took a success told at once, while the user still had Megler's question before them, for a failed install
// ("installed version -1 is not equal to desired version") -- or what Megler wrote of it in the apps folder's installs/
// (Native.appInstall): the user's no (STATUS_FAILURE_ABORTED), an app that cannot run here (STATUS_FAILURE_INCOMPATIBLE),
// an install that failed (STATUS_FAILURE). Then the session is gone, its files too. ACTION_PACKAGE_ADDED (and
// ACTION_PACKAGE_REPLACED for an update) comes before, as on Android, from megler.PhoneApps, which has just seen the app.
//
// And an app uninstalled, as Android's package installer uninstalls one: PackageInstaller.uninstall (Google Play's
// Uninstall), or an intent, ACTION_DELETE or ACTION_UNINSTALL_PACKAGE of package:<name>. Megler is asked
// (Native.uninstallApp), and asks the user; the app is gone when the phone's apps no longer have it (ACTION_PACKAGE_REMOVED
// from megler.PhoneApps, then STATUS_SUCCESS), and the user's no is installs/<package>.uninstall.txt
// (STATUS_FAILURE_ABORTED). Google Play showed "Uninstalling..." for ever: the call did nothing. Google Play itself asks
// through PackageManager.deletePackage, with an IPackageDeleteObserver of its own (deleteObserved), which is told
// packageDeleted as Android tells it: DELETE_SUCCEEDED, DELETE_FAILED_ABORTED for the user's no.
//
// What a run leaves when its process ends before a session's outcome -- the session's files, a handed APK's copy -- the next
// run removes at its start (sweep).
package megler;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class Installs {
    static final String APK_TYPE = "application/vnd.android.package-archive";
    private Installs() {}

    /** Whether `i` asks to uninstall an app (package:<name>), and no activity of the app's own is named for it. */
    public static boolean isUninstall(Intent i) {
        if (i == null || i.getComponent() != null || i.getData() == null || !"package".equals(i.getData().getScheme())) return false;
        return Intent.ACTION_DELETE.equals(i.getAction()) || "android.intent.action.UNINSTALL_PACKAGE".equals(i.getAction());
    }

    /** Whether `i` asks to install an APK, and no activity of the app's own is named for it. */
    public static boolean isInstall(Intent i) {
        if (i == null || i.getComponent() != null) return false;
        Uri u = i.getData();
        if (u == null || !("file".equals(u.getScheme()) || "content".equals(u.getScheme()))) return false;
        if ("android.intent.action.INSTALL_PACKAGE".equals(i.getAction())) return true;
        if (!Intent.ACTION_VIEW.equals(i.getAction())) return false;
        String type = i.getType();
        if (APK_TYPE.equals(type)) return true;
        String path = u.getPath() == null ? "" : u.getPath().toLowerCase(java.util.Locale.ROOT);
        return path.endsWith(".apk") || path.endsWith(".apks") || path.endsWith(".xapk");
    }

    /** Hands the APK of an install intent to Megler, off the main thread when it is to be copied out of a provider. */
    public static void install(final Context from, Intent i) {
        final Uri u = i.getData();
        if ("file".equals(u.getScheme())) {
            String path = u.getPath();
            Native.installApk(path, new File(path).getName());
            return;
        }
        new Thread(() -> {
            String name = nameOf(u);
            File dir = new File(from.getCacheDir(), "megler-install");
            dir.mkdirs();
            File out = new File(dir, name);
            try (InputStream in = from.getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(out)) {
                if (in == null) throw new java.io.FileNotFoundException(u.toString());
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) o.write(buf, 0, n);
            } catch (Exception e) {
                android.util.Log.w("Megler", "install: " + u + " could not be read: " + e);
                return;
            }
            Native.installApk(out.getPath(), name);
        }, "Megler install").start();
    }

    /** A file's name for a content:// URI: its last part, made an .apk's when it has no such ending. */
    private static String nameOf(Uri u) {
        String last = u.getLastPathSegment();
        if (last == null || last.isEmpty()) last = "app";
        last = last.replace('/', '_').replace('\\', '_');
        String lower = last.toLowerCase(java.util.Locale.ROOT);
        return lower.endsWith(".apk") || lower.endsWith(".apks") || lower.endsWith(".xapk") ? last : last + ".apk";
    }

    // ---- PackageInstaller's sessions

    /** A session: its folder in the app's cache, the files written into it, what it was made for. */
    public static final class Session {
        final int id;
        final File dir;
        final String packageName;
        boolean committed;
        /*
         * The names the app wrote its files under (openWrite), and the file each is in the folder: getNames gives the names
         * back as they were written, as Android's does. Play Store checks them against the artifacts it wrote
         * ("config.armeabi_v7a"), and its every install ended "Session doesn't contain resources of required artifacts"
         * (status 6573) while the folder's names, an .apk added, were given (package 554).
         */
        final java.util.LinkedHashMap<String, String> written = new java.util.LinkedHashMap<>();
        Session(int id, File dir, String packageName) { this.id = id; this.dir = dir; this.packageName = packageName; }
        public int id() { return id; }
        public String packageName() { return packageName; }
        public boolean committed() { return committed; }
    }

    private static final java.util.HashMap<Integer, Session> sSessions = new java.util.HashMap<>();
    private static int sNext = 1;

    public static synchronized int create(String packageName) {
        Context app = AppRunner.application();
        int id = sNext++;
        File dir = new File(app != null ? app.getCacheDir() : new File("/data/local/tmp"), "megler-session-" + id);
        deleteTree(dir);
        dir.mkdirs();
        sSessions.put(id, new Session(id, dir, packageName));
        android.util.Log.i("Megler", "install: session " + id + " made" + (packageName != null ? " for " + packageName : ""));
        return id;
    }

    public static synchronized Session session(int id) { return sSessions.get(id); }

    public static synchronized java.util.List<Integer> ids() { return new java.util.ArrayList<>(sSessions.keySet()); }

    public static OutputStream openWrite(int id, String name) throws java.io.IOException {
        Session s = session(id);
        if (s == null) throw new SecurityException("no session " + id);
        String file = name.replace('/', '_').replace('\\', '_');
        String lower = file.toLowerCase(java.util.Locale.ROOT);
        if (!lower.endsWith(".apk") && !lower.endsWith(".dm")) file += ".apk";
        synchronized (s.written) { s.written.put(name, file); }
        return new FileOutputStream(new File(s.dir, file));
    }

    public static String[] names(int id) {
        Session s = session(id);
        if (s == null) return new String[0];
        synchronized (s.written) {
            if (!s.written.isEmpty()) return s.written.keySet().toArray(new String[0]);
        }
        String[] list = s.dir.list();
        return list != null ? list : new String[0];
    }

    /** A file the app wrote into the session, by the name it wrote it under (Session.openRead); null when there is none. */
    public static java.io.InputStream openRead(int id, String name) throws java.io.IOException {
        Session s = session(id);
        if (s == null || name == null) return null;
        String file;
        synchronized (s.written) { file = s.written.get(name); }
        File f = new File(s.dir, file != null ? file : name.replace('/', '_').replace('\\', '_'));
        return f.isFile() ? new java.io.FileInputStream(f) : null;
    }

    /** The session's files to Megler: one APK, or the folder of one app's; then, once it has an outcome, that to `status`. */
    public static void commit(int id, android.content.IntentSender status) {
        Session s = session(id);
        if (s == null || s.committed) return;
        s.committed = true;
        File[] files = s.dir.listFiles((d, n) -> n.toLowerCase(java.util.Locale.ROOT).endsWith(".apk"));
        if (files == null) files = new File[0];
        File base = files.length > 0 ? files[0] : null;
        for (File f : files) if (f.getName().equalsIgnoreCase("base.apk")) base = f;
        android.content.pm.PackageInfo apk = base != null ? ArchiveInfo.read(base.getPath()) : null;
        String pkg = s.packageName != null ? s.packageName : apk != null ? apk.packageName : null;
        // A phone maker's list of apps to install at setup (Play Store's restore of "android.autoinstalls.config.google.nexus"):
        // nothing an app on this phone needs, and the user is not asked about it; Play Store goes on without it.
        if (pkg != null && pkg.startsWith("android.autoinstalls.config.")) {
            android.util.Log.i("Megler", "install: session " + id + " for " + pkg + ", a phone maker's list of apps for setup, not installed");
            finish(s, status, pkg, 3 /* STATUS_FAILURE_ABORTED */, "not for this phone");
            return;
        }
        long want = apk != null ? apk.versionCode & 0xffffffffL : -1;
        String name = (pkg != null ? pkg : "app") + (files.length > 1 ? ".apks" : ".apk");
        // what there is before Megler has it: the app at its version, if the phone has it, and what Megler last told of the name
        PhoneApps.App had = PhoneApps.find(pkg);
        long hadVersion = had != null ? had.versionCode : -1;
        String told = outcome(name);
        boolean handed = false;
        if (files.length == 1) handed = Native.installApk(files[0].getPath(), name);
        else if (files.length > 1) {
            File bundle = new File(s.dir.getParentFile(), s.dir.getName() + ".apks");
            try {
                bundle(files, bundle);
                handed = Native.installApk(bundle.getPath(), name);
            } catch (java.io.IOException e) {
                android.util.Log.w("Megler", "install: session " + id + "'s files could not be put in one bundle: " + e);
                bundle.delete();
            }
        }
        android.util.Log.i("Megler", "install: session " + id + " committed, " + files.length + " file(s) as " + name + ": "
            + (handed ? "Megler has it" + (want >= 0 ? ", version " + want + " awaited" : "") : "Megler could not take it"));
        if (!handed) {
            finish(s, status, pkg, 1 /* STATUS_FAILURE */, "Megler could not take it");
            return;
        }
        new Thread(() -> await(s, status, pkg, name, want, hadVersion, told), "Megler install " + id).start();
    }

    /* How long a session waits for its outcome: the user's yes in Megler, then the install of an app of the phone's own. */
    private static final long AWAIT_MS = 30 * 60 * 1000L;

    /* Waits for the session's outcome (as the header tells), and tells it. */
    private static void await(Session s, android.content.IntentSender status, String pkg, String name, long want, long hadVersion, String told) {
        long until = android.os.SystemClock.uptimeMillis() + AWAIT_MS;
        while (android.os.SystemClock.uptimeMillis() < until) {
            PhoneApps.App a = PhoneApps.find(pkg);
            if (a != null && (want >= 0 ? (a.versionCode & 0xffffffffL) == want : a.versionCode != hadVersion)) {
                finish(s, status, pkg, 0 /* STATUS_SUCCESS */, "Megler installed it");
                return;
            }
            String now = outcome(name);
            if (now != null && !now.equals(told)) {
                // "<when>\t<what>\t<why>"
                String[] f = now.trim().split("\t", 3);
                String what = f.length > 1 ? f[1] : "failed", why = f.length > 2 ? f[2] : what;
                int code = "declined".equals(what) ? 3 /* STATUS_FAILURE_ABORTED */ : "incompatible".equals(what) ? 7 /* STATUS_FAILURE_INCOMPATIBLE */ : 1;
                finish(s, status, pkg, code, why);
                return;
            }
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                break;
            }
        }
        finish(s, status, pkg, 1 /* STATUS_FAILURE */, "Megler did not install it in " + AWAIT_MS / 60000 + " minutes");
    }

    /** The app `pkg` uninstalled by Megler, once the user says yes; then its outcome to `status` (null: to no one). */
    public static void uninstall(String pkg, android.content.IntentSender status) {
        uninstall(pkg, (code, message) -> tell(status, -1, pkg, code, message));
    }

    /** What an uninstall ended with: a PackageInstaller status (STATUS_SUCCESS, STATUS_FAILURE_ABORTED, STATUS_FAILURE). */
    public interface Uninstalled { void done(int status, String message); }

    /**
     * The app `pkg` uninstalled for PackageManager.deletePackage: `observer`, an IPackageDeleteObserver (the app's own
     * Stub; the platform's interface here has no methods), is told packageDeleted(pkg, returnCode) on the main thread.
     */
    public static void deleteObserved(String pkg, Object observer) {
        uninstall(pkg, (code, message) -> new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            if (observer == null) return;
            int returnCode = code == 0 ? 1 /* DELETE_SUCCEEDED */ : code == 3 ? -5 /* DELETE_FAILED_ABORTED */ : -1 /* DELETE_FAILED_INTERNAL_ERROR */;
            try {
                observer.getClass().getMethod("packageDeleted", String.class, int.class).invoke(observer, pkg, returnCode);
            } catch (ReflectiveOperationException e) {
                android.util.Log.w("Megler", "uninstall: " + pkg + "'s observer could not be told: " + e);
            }
        }));
    }

    /** The app `pkg` uninstalled by Megler, once the user says yes; then its outcome to `done`. */
    public static void uninstall(String pkg, Uninstalled done) {
        if (pkg == null || PhoneApps.find(pkg) == null) {
            android.util.Log.i("Megler", "uninstall: " + pkg + " is not one of the phone's apps");
            done.done(1 /* STATUS_FAILURE */, "Not installed");
            return;
        }
        String name = pkg + ".uninstall";
        String told = outcome(name);
        if (!Native.uninstallApp(pkg)) {
            done.done(1 /* STATUS_FAILURE */, "Megler could not be asked");
            return;
        }
        android.util.Log.i("Megler", "uninstall: " + pkg + " asked of Megler");
        new Thread(() -> {
            long until = android.os.SystemClock.uptimeMillis() + AWAIT_MS;
            while (android.os.SystemClock.uptimeMillis() < until) {
                if (PhoneApps.find(pkg) == null) {
                    android.util.Log.i("Megler", "uninstall: " + pkg + " is gone");
                    done.done(0 /* STATUS_SUCCESS */, "Megler uninstalled it");
                    return;
                }
                String now = outcome(name);
                if (now != null && !now.equals(told)) {
                    String[] f = now.trim().split("\t", 3);
                    String what = f.length > 1 ? f[1] : "failed", why = f.length > 2 ? f[2] : what;
                    android.util.Log.i("Megler", "uninstall: " + pkg + " not uninstalled: " + why);
                    done.done("declined".equals(what) ? 3 /* STATUS_FAILURE_ABORTED */ : 1, why);
                    return;
                }
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    break;
                }
            }
            done.done(1 /* STATUS_FAILURE */, "Megler did not uninstall it in " + AWAIT_MS / 60000 + " minutes");
        }, "Megler uninstall " + pkg).start();
    }

    /* What Megler last wrote of the APK by that name, or null. */
    private static String outcome(String name) {
        byte[] b = Native.appInstall(name);
        return b != null ? new String(b, java.nio.charset.StandardCharsets.UTF_8) : null;
    }

    /* The session's outcome to its IntentSender, on the main thread; the session and its files gone. */
    private static void finish(Session s, android.content.IntentSender status, String pkg, int code, String message) {
        synchronized (Installs.class) { sSessions.remove(s.id); }
        deleteTree(s.dir);
        new File(s.dir.getParentFile(), s.dir.getName() + ".apks").delete();
        android.util.Log.i("Megler", "install: session " + s.id + (code == 0 ? " installed: " : " not installed (status " + code + "): ") + message);
        tell(status, s.id, pkg, code, message);
    }

    /* A status to its IntentSender, on the main thread, as Android's package installer sends one (a session's id when it is one). */
    private static void tell(android.content.IntentSender status, int session, String pkg, int code, String message) {
        if (status == null) return;
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            Intent result = new Intent();
            if (session >= 0) result.putExtra("android.content.pm.extra.SESSION_ID", session);
            result.putExtra("android.content.pm.extra.STATUS", code);
            result.putExtra("android.content.pm.extra.STATUS_MESSAGE", message);
            if (pkg != null) result.putExtra("android.content.pm.extra.PACKAGE_NAME", pkg);
            try {
                status.sendIntent(AppRunner.application(), 0, result, null, null);
            } catch (Exception e) {
                android.util.Log.w("Megler", "install: a status could not be sent: " + e);
            }
        });
    }

    /* The APKs into one .apks file: a ZIP of them, each stored as it is (an APK is compressed inside already). */
    private static void bundle(File[] files, File into) throws java.io.IOException {
        byte[] chunk = new byte[1 << 16];
        try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(new java.io.BufferedOutputStream(new FileOutputStream(into), 1 << 16))) {
            for (File f : files) {
                java.util.zip.CRC32 crc = new java.util.zip.CRC32();
                try (InputStream in = new java.io.FileInputStream(f)) {
                    for (int n; (n = in.read(chunk)) > 0; ) crc.update(chunk, 0, n);
                }
                java.util.zip.ZipEntry e = new java.util.zip.ZipEntry(f.getName());
                e.setMethod(java.util.zip.ZipEntry.STORED);
                e.setSize(f.length());
                e.setCompressedSize(f.length());
                e.setCrc(crc.getValue());
                zip.putNextEntry(e);
                try (InputStream in = new java.io.FileInputStream(f)) {
                    for (int n; (n = in.read(chunk)) > 0; ) zip.write(chunk, 0, n);
                }
                zip.closeEntry();
            }
        }
    }

    public static synchronized void abandon(int id) {
        Session s = sSessions.remove(id);
        if (s != null && !s.committed) deleteTree(s.dir);
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static long sizeOf(File f) {
        File[] kids = f.listFiles();
        if (kids == null) return f.length();
        long n = 0;
        for (File k : kids) n += sizeOf(k);
        return n;
    }

    /*
     * What earlier runs of the app left in its cache, removed once at its start, off the main thread: a session's folder and
     * its bundle (megler-session-<id>, .apks), which stay when the process ends before the session has its outcome -- Update
     * All closes the app, the user leaves it -- and the copies of APKs install intents handed over (megler-install). Play
     * Store kept a Gmail update's session so, 228 MB of a Lumia 650's last 600 (package 663). What this run has made since
     * its start is newer than that, and stays.
     */
    public static void sweep(final Context app) {
        final long started = System.currentTimeMillis();
        Thread t = new Thread(() -> {
            File cache = app.getCacheDir();
            if (cache == null) return;
            long freed = 0;
            int count = 0;
            File[] old = cache.listFiles((d, n) -> n.startsWith("megler-session-"));
            if (old != null) for (File f : old) {
                if (f.lastModified() >= started) continue;
                String id = f.getName().substring("megler-session-".length());
                if (id.endsWith(".apks")) id = id.substring(0, id.length() - 5);
                try {
                    synchronized (Installs.class) { if (sSessions.containsKey(Integer.parseInt(id))) continue; }
                } catch (NumberFormatException e) {
                    continue;
                }
                freed += sizeOf(f);
                deleteTree(f);
                count++;
            }
            File[] handed = new File(cache, "megler-install").listFiles();
            if (handed != null) for (File f : handed) {
                if (f.lastModified() >= started) continue;
                freed += sizeOf(f);
                deleteTree(f);
                count++;
            }
            if (count > 0)
                android.util.Log.i("Megler", "install: what " + count + " installs of earlier runs left in the cache removed, " + (freed >> 20) + " MB");
        }, "Megler install sweep");
        t.setPriority(Thread.MIN_PRIORITY);
        t.start();
    }
}
