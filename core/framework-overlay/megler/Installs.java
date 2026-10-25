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
// processor). A committed session is told STATUS_SUCCESS through its IntentSender once Megler has it: the user still says
// yes in Megler, and the app learns no more than that it was handed over.
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
        Session(int id, File dir, String packageName) { this.id = id; this.dir = dir; this.packageName = packageName; }
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
        if (!file.toLowerCase(java.util.Locale.ROOT).endsWith(".apk")) file += ".apk";
        return new FileOutputStream(new File(s.dir, file));
    }

    public static String[] names(int id) {
        Session s = session(id);
        String[] list = s == null ? null : s.dir.list();
        return list != null ? list : new String[0];
    }

    /** The session's files to Megler: one APK, or the folder of one app's; then STATUS_SUCCESS (or FAILURE) to `status`. */
    public static void commit(int id, android.content.IntentSender status) {
        Session s = session(id);
        if (s == null || s.committed) return;
        s.committed = true;
        File[] files = s.dir.listFiles();
        boolean handed = false;
        if (files != null && files.length == 1) handed = Native.installApk(files[0].getPath(), files[0].getName());
        else if (files != null && files.length > 1) {
            File bundle = new File(s.dir.getParentFile(), s.dir.getName() + ".apks");
            String name = (s.packageName != null ? s.packageName : "app") + ".apks";
            try {
                bundle(files, bundle);
                handed = Native.installApk(bundle.getPath(), name);
            } catch (java.io.IOException e) {
                android.util.Log.w("Megler", "install: session " + id + "'s files could not be put in one bundle: " + e);
                bundle.delete();
            }
        }
        android.util.Log.i("Megler", "install: session " + id + " committed, " + (files == null ? 0 : files.length) + " file(s): "
            + (handed ? "Megler has it" : "Megler could not take it"));
        if (status == null) return;
        Intent result = new Intent();
        result.putExtra("android.content.pm.extra.SESSION_ID", id);
        result.putExtra("android.content.pm.extra.STATUS", handed ? 0 /* STATUS_SUCCESS */ : 1 /* STATUS_FAILURE */);
        result.putExtra("android.content.pm.extra.STATUS_MESSAGE", handed ? "Megler installs it" : "Megler could not take it");
        if (s.packageName != null) result.putExtra("android.content.pm.extra.PACKAGE_NAME", s.packageName);
        try {
            status.sendIntent(AppRunner.application(), 0, result, null, null);
        } catch (Exception e) {
            android.util.Log.w("Megler", "install: the session's status could not be sent: " + e);
        }
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
}
