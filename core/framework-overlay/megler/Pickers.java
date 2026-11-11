// Megler: a class of this framework's own, new. The phone's own picker of files and its camera, for the intents an app sends to
// Android's: its documents UI and gallery -- ACTION_GET_CONTENT, ACTION_OPEN_DOCUMENT, ACTION_PICK of pictures, videos or
// sounds, Android 13's photo picker (MediaStore.ACTION_PICK_IMAGES, and androidx's and Google Play services' actions of it) --
// and its camera app (MediaStore.ACTION_IMAGE_CAPTURE, ACTION_VIDEO_CAPTURE; INTENT_ACTION_STILL_IMAGE_CAMERA and
// INTENT_ACTION_VIDEO_CAMERA, which open Windows' camera app). Megler had none of them: startActivity threw
// ActivityNotFoundException ("No Activity found to handle"), and every app's "attach a file", "choose a profile picture" and
// "take a photo" said there was no app for it or did nothing (0.1.0.690, the users' two phones).
//
// How a request goes:
//  - AppRunner.startActivity hands an intent here (start) before it looks among the activities of the app's own, of Google
//    Play services' and of the phone's other apps: an intent that names no package, or a package the phone has no app of (the
//    documents UI's, the camera's, Google Play services' photo picker), is the system's, as on a phone where those are system
//    apps. The same intents wrapped in ACTION_CHOOSER come here once AppRunner has unwrapped them (EXTRA_INITIAL_INTENTS
//    left out: a camera an app adds to its chooser of a gallery is the user's to open from the app's own button).
//  - The shell is asked (Native.pick, megler_system_host.pick) for Windows' FileOpenPicker -- filtered by the intent's types,
//    one file or several, opening in the pictures, videos or music library for an app that takes those -- or its
//    CameraCaptureUI. It puts a copy of what the user picked or took into a folder of the app's data made for the request
//    (megler-picked/<request>), and answers.
//  - The answer comes to the main thread as an event (AppRunner.deliver, answered) and goes to the activity that asked once the
//    app is in front again (resumed: the picker hid its window, as another app's activity hides the caller on Android, and the
//    result comes before the caller resumes), as onActivityResult: RESULT_OK with the files' content: URIs, or
//    RESULT_CANCELED. An activity made again meanwhile (the screen turned under the picker) is the one that hears it (replaced).
//  - The URIs are this provider's (Provider, authority megler.picked): content://megler.picked/<request>/<name>, with
//    OpenableColumns' _display_name and _size, mime_type, the photo picker's columns (PickerMediaColumns: datetaken, width,
//    height, orientation, duration_millis), the documents' (document_id, last_modified, flags) and _data, the copy's own
//    path, for an app that reads a picked file by its path. Read only: a write would change a copy the user never sees.
//    The authority is none of Android's providers', whose ids apps take apart (a media document's "image:1234" read as a
//    MediaStore row, an external storage document's "primary:Download/x.pdf" as a path), which these files are not; nor
//    MediaStore's "media". It names where the files come from in an app's log.
//  - A camera's photo goes where the app said (MediaStore.EXTRA_OUTPUT, its own FileProvider's URI as a rule), through the
//    app's ContentResolver, copied on a thread of its own, and the copy here goes; RESULT_OK with no data, as Android's camera
//    answers. Without EXTRA_OUTPUT the result has a small Bitmap in its "data" extra, of at most 50,000 pixels as Android's
//    camera makes it (CameraUtil.makeBitmap), and the photo goes. A video is likewise copied where the app said, the result's
//    data that URI; without EXTRA_OUTPUT its data is the video here.
//  - Copies, not the files where they are: an app reads a picked file after the picker has gone -- at once on a thread of
//    its own, or the next day from a link it kept (takePersistableUriPermission) -- and the access Windows' broker gives to a
//    picked file is the shell's alone, through the StorageFile the picker handed it, and to no path. The folders of requests
//    older than a week go when the next request is made.
//  - A WebView's <input type=file> comes here whether or not the app handles it (forWebView, from WebChromeClient's
//    onShowFileChooser, which Android leaves doing nothing): the page's accept types, several files where it takes them, and
//    the camera where it asks for capture.
// The log says what was asked, what Windows answered and what the app was given -- the files' extensions, never their names.
package megler;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.webkit.MimeTypeMap;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;

public final class Pickers {
    private Pickers() {}
    private static final String TAG = "MeglerPickers";
    /** The authority of the files picked and taken here: content://megler.picked/<request>/<name>. */
    public static final String AUTHORITY = "megler.picked";

    /* What an intent asks for: files (Windows' picker), a photo or a video (its camera), or the camera app itself. The first
       three are Native.pick's kinds. */
    static final int NONE = -1, FILES = 0, PHOTO = 1, VIDEO = 2, CAMERA_APP = 3;
    /* The shell's answers (megler.h, MEGLER_PICK_*). */
    private static final int OK = 0, CANCELED = 1;
    static final String ACTION_PICK_IMAGES = "android.provider.action.PICK_IMAGES",
        ACTION_PICK_IMAGES_ANDROIDX = "androidx.activity.result.contract.action.PICK_IMAGES",
        ACTION_PICK_IMAGES_GMS = "com.google.android.gms.provider.action.PICK_IMAGES",
        EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX",
        ACTION_IMAGE_CAPTURE = "android.media.action.IMAGE_CAPTURE", ACTION_IMAGE_CAPTURE_SECURE = "android.media.action.IMAGE_CAPTURE_SECURE",
        ACTION_VIDEO_CAPTURE = "android.media.action.VIDEO_CAPTURE",
        ACTION_STILL_IMAGE_CAMERA = "android.media.action.STILL_IMAGE_CAMERA", ACTION_STILL_IMAGE_CAMERA_SECURE = "android.media.action.STILL_IMAGE_CAMERA_SECURE",
        ACTION_VIDEO_CAMERA = "android.media.action.VIDEO_CAMERA",
        EXTRA_OUTPUT = "output", EXTRA_VIDEO_QUALITY = "android.intent.extra.videoQuality", EXTRA_DURATION_LIMIT = "android.intent.extra.durationLimit";
    /* The most pixels of the photo a camera hands back without EXTRA_OUTPUT, as Android's camera app makes it (50 * 1024). */
    private static final int INLINE_PIXELS = 50 * 1024;
    /* A request's folder older than this goes when the next request is made. */
    private static final long KEPT_MS = 7L * 24 * 3600 * 1000;

    // ---- which intents

    /** What an intent asks of Android's documents UI, gallery, photo picker or camera app; NONE for anything else. */
    static int kind(Intent i) {
        String a = i == null ? null : i.getAction();
        if (a == null) return NONE;
        switch (a) {
        case Intent.ACTION_GET_CONTENT: {
            // (a contact or a phone number, vnd.android.cursor.item/..., is the contacts app's)
            String t = i.getType();
            return t != null && t.startsWith("vnd.android.cursor") ? NONE : FILES;
        }
        case Intent.ACTION_OPEN_DOCUMENT:
            return FILES;
        case Intent.ACTION_PICK:
            return mediaPick(i) ? FILES : NONE;
        case ACTION_PICK_IMAGES: case ACTION_PICK_IMAGES_ANDROIDX: case ACTION_PICK_IMAGES_GMS:
            return FILES;
        case ACTION_IMAGE_CAPTURE: case ACTION_IMAGE_CAPTURE_SECURE:
            return PHOTO;
        case ACTION_VIDEO_CAPTURE:
            return VIDEO;
        case ACTION_STILL_IMAGE_CAMERA: case ACTION_STILL_IMAGE_CAMERA_SECURE: case ACTION_VIDEO_CAMERA:
            return CAMERA_APP;
        default:
            return NONE;
        }
    }

    /* ACTION_PICK of something a gallery has (a type of pictures, videos or sounds, or MediaStore's URI of them); not of a
       contact (ContactsContract's URI), which is the contacts app's. */
    private static boolean mediaPick(Intent i) {
        String t = i.getType();
        if (t != null) return t.startsWith("image/") || t.startsWith("video/") || t.startsWith("audio/") || t.equals("*/*");
        Uri d = i.getData();
        return d != null && "media".equals(d.getAuthority());
    }

    /**
     * What an intent asks of the system's picker or camera, NONE when it is not for them: one that names the app's own package,
     * or one of the phone's other apps (Google Photos where the phone has it as an app), is that app's. One that names another
     * package -- the documents UI's or the camera's, as an app that started what resolveActivity answered does; Google Play
     * services' photo picker -- is the system's here.
     */
    static int request(Intent i) {
        int k = kind(i);
        if (k == NONE) return NONE;
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        if (pkg != null && !GooglePackages.GMS.equals(pkg) && (pkg.equals(Native.packageName()) || PhoneApps.find(pkg) != null)) return NONE;
        return k;
    }

    /** The MIME types an intent takes: EXTRA_MIME_TYPES, else its type (some apps list several, comma-separated), else a
     *  MediaStore URI's kind; the photo picker's pictures and videos alone; any file ("*" + "/*") when it says none. */
    static ArrayList<String> types(Intent i, int kind) {
        ArrayList<String> l = new ArrayList<>();
        if (kind == PHOTO) { l.add("image/jpeg"); return l; }
        if (kind == VIDEO) { l.add("video/mp4"); return l; }
        String[] extra = null;
        try { extra = i.getStringArrayExtra(Intent.EXTRA_MIME_TYPES); } catch (RuntimeException e) {}
        if (extra != null) for (String t : extra) addType(l, t);
        if (l.isEmpty() && i.getType() != null) for (String t : i.getType().split(",")) addType(l, t);
        if (l.isEmpty() && Intent.ACTION_PICK.equals(i.getAction()) && i.getData() != null && i.getData().getPath() != null) {
            String p = i.getData().getPath();
            addType(l, p.contains("/images") ? "image/*" : p.contains("/video") ? "video/*" : p.contains("/audio") ? "audio/*" : "*/*");
        }
        if (photoPicker(i.getAction())) {
            // Android's photo picker shows pictures and videos alone, of the types asked for; none asked, both.
            for (int k = l.size() - 1; k >= 0; k--) if (!l.get(k).startsWith("image/") && !l.get(k).startsWith("video/")) l.remove(k);
            if (l.isEmpty()) { l.add("image/*"); l.add("video/*"); }
        }
        if (l.isEmpty() || l.contains("*/*")) { l.clear(); l.add("*/*"); }
        return l;
    }

    private static boolean photoPicker(String action) {
        return ACTION_PICK_IMAGES.equals(action) || ACTION_PICK_IMAGES_ANDROIDX.equals(action) || ACTION_PICK_IMAGES_GMS.equals(action);
    }

    /* A type as an intent or a page's accept attribute has it: "image/*", "IMAGE/PNG", ".pdf" (a page's), "image", "*". */
    private static void addType(ArrayList<String> l, String t) {
        if (t == null) return;
        t = t.trim().toLowerCase(Locale.ROOT);
        if (t.isEmpty()) return;
        if (t.startsWith(".")) {
            String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(t.substring(1));
            t = m != null ? m : "*/*";
        }
        if (t.equals("*")) t = "*/*";
        if (t.indexOf('/') < 0) t = t + "/*";
        if (!l.contains(t)) l.add(t);
    }

    /* Where Windows' picker opens (megler.h, MEGLER_PICK_START_*): the library of an app's pictures, videos or sounds, else the documents. */
    private static int startIn(List<String> types) {
        boolean image = false, video = false, audio = false, other = false;
        for (String t : types) {
            if (t.startsWith("image/")) image = true;
            else if (t.startsWith("video/")) video = true;
            else if (t.startsWith("audio/")) audio = true;
            else other = true;
        }
        return other ? 0 : image ? 1 : video ? 2 : audio ? 3 : 0;
    }

    // ---- the system's activities, for the package manager

    /**
     * What PackageManager.resolveActivity and queryIntentActivities answer for an intent the system's picker or camera takes
     * here, as a phone answers with its own: Android's documents UI (com.android.documentsui) for files, its camera app
     * (com.android.camera2) for photos and videos -- where the device has a camera, as a phone without one has no camera app.
     * Null for any other intent. An app asks before it offers the button (resolveActivity non-null), and some start what it was
     * answered by its package and class (androidx's photo picker of a phone's own, PickVisualMedia): AppRunner.startActivity
     * takes that here likewise (request). A system package of the phone's, not "android": that one stands for Android's own
     * chooser, which an app takes to mean several apps answer.
     */
    public static ResolveInfo resolveInfo(Intent i) {
        int k = request(i);
        if (k == NONE) return null;
        if (k != FILES && (CameraNative.features() & CameraNative.FEATURE_ANY) == 0) return null;
        String pkg = k == FILES ? "com.android.documentsui" : "com.android.camera2";
        String cls = k == FILES ? "com.android.documentsui.picker.PickActivity" : k == PHOTO ? "com.android.camera.CaptureActivity"
            : k == VIDEO ? "com.android.camera.VideoCamera" : "com.android.camera.CameraActivity";
        String label = k == FILES ? "Files" : "Camera";
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = pkg;
        app.processName = pkg;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        app.enabled = true;
        app.nonLocalizedLabel = label;
        ActivityInfo a = new ActivityInfo();
        a.packageName = pkg;
        a.name = cls;
        a.exported = true;
        a.enabled = true;
        a.nonLocalizedLabel = label;
        a.applicationInfo = app;
        ResolveInfo r = new ResolveInfo();
        r.activityInfo = a;
        r.nonLocalizedLabel = label;
        r.isDefault = true;
        r.match = 0x00108000;   /* IntentFilter.MATCH_CATEGORY_TYPE | MATCH_ADJUSTMENT_NORMAL */
        return r;
    }

    // ---- requests

    /** A request made, from start to the answer given. */
    private static final class Pending {
        int id = -1, kind;
        String action;
        Activity caller;
        int request = -1;
        ArrayList<String> types;
        boolean multiple;
        int max, durationS, quality = -1;
        Uri output;
        ValueCallback<Uri[]> web;
        File folder;
        int status = -1;
        String[] names = new String[0];
        String what() { return "request " + id + " (" + (action != null ? action : "a WebView's file input") + " of " + String.join(" ", types) + ")"; }
    }
    /* Asked of the shell, by number; answered and waiting for the app to be in front. Main thread only. */
    private static final HashMap<Integer, Pending> sAsked = new HashMap<>();
    private static final ArrayList<Pending> sAnswered = new ArrayList<>();
    private static Handler sHandler;
    private static int sNextId;
    private static boolean sSaidWaiting;

    private static Handler handler() {
        if (sHandler == null) sHandler = new Handler(Looper.getMainLooper());
        return sHandler;
    }
    private static boolean onMain() { return Looper.myLooper() == Looper.getMainLooper(); }

    /**
     * An intent of the app's (Context.startActivity, startActivityForResult) for the system's picker or camera: asked of the
     * shell, the result to come to `caller` with `request` when that is not negative. False for any other intent, which goes
     * where it went.
     */
    public static boolean start(Activity caller, Intent i, int request) {
        int kind = request(i);
        if (kind == NONE) return false;
        if (kind == CAMERA_APP) {
            // The phone's camera app itself (no result but the user's Back), as the location settings are opened: Windows' own.
            Log.i(TAG, "startActivity " + i.getAction() + " -> Windows' camera app (microsoft.windows.camera:)");
            Native.openUrl("microsoft.windows.camera:");
            AppRunner.awaitWindowsPage(caller, request);
            return true;
        }
        final Pending p = new Pending();
        p.kind = kind;
        p.action = i.getAction();
        p.caller = caller;
        p.request = request;
        p.types = types(i, kind);
        if (kind == FILES) {
            int max = 0;
            try { max = i.getIntExtra(EXTRA_PICK_IMAGES_MAX, 0); } catch (RuntimeException e) {}
            boolean multiple = false;
            try { multiple = i.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false); } catch (RuntimeException e) {}
            p.multiple = multiple || max > 1;
            p.max = max > 1 ? max : 0;
        } else {
            Object out = null;
            try { out = i.getParcelableExtra(EXTRA_OUTPUT); } catch (RuntimeException e) {}
            // (an app's own FileProvider's URI as a rule; a file: URI from an old one; a String of either from a careless one)
            if (out instanceof Uri) p.output = (Uri) out;
            else if (out instanceof String) p.output = Uri.parse((String) out);
            if (kind == VIDEO) {
                try { p.durationS = i.getIntExtra(EXTRA_DURATION_LIMIT, 0); } catch (RuntimeException e) {}
                try { p.quality = i.hasExtra(EXTRA_VIDEO_QUALITY) ? i.getIntExtra(EXTRA_VIDEO_QUALITY, 1) : -1; } catch (RuntimeException e) {}
            }
        }
        if (onMain()) ask(p);
        else handler().post(() -> ask(p));
        return true;
    }

    /**
     * A WebView's <input type=file> (WebChromeClient.onShowFileChooser, where the app's client leaves it, or the app has none):
     * the files the page takes -- its accept types, several where it has `multiple` -- or the camera where it asks for capture
     * of pictures or videos alone. True: `callback` hears the URIs, or null when the user backed out, as a page's chooser does.
     */
    public static boolean forWebView(ValueCallback<Uri[]> callback, WebChromeClient.FileChooserParams params) {
        if (callback == null || params == null) return false;
        int mode = params.getMode();
        if (mode != WebChromeClient.FileChooserParams.MODE_OPEN && mode != WebChromeClient.FileChooserParams.MODE_OPEN_MULTIPLE) return false;
        final Pending p = new Pending();
        p.web = callback;
        ArrayList<String> types = new ArrayList<>();
        String[] accept = params.getAcceptTypes();
        if (accept != null) for (String a : accept) if (a != null) for (String t : a.split(",")) addType(types, t);
        if (types.isEmpty() || types.contains("*/*")) { types.clear(); types.add("*/*"); }
        boolean images = true, videos = true;
        for (String t : types) { images &= t.startsWith("image/"); videos &= t.startsWith("video/"); }
        p.kind = params.isCaptureEnabled() && images ? PHOTO : params.isCaptureEnabled() && videos ? VIDEO : FILES;
        p.types = p.kind == PHOTO ? new ArrayList<>(java.util.Collections.singletonList("image/jpeg"))
            : p.kind == VIDEO ? new ArrayList<>(java.util.Collections.singletonList("video/mp4")) : types;
        p.multiple = p.kind == FILES && mode == WebChromeClient.FileChooserParams.MODE_OPEN_MULTIPLE;
        if (onMain()) ask(p);
        else handler().post(() -> ask(p));
        return true;
    }

    /* The request asked of the shell: its number, its folder, Native.pick. Main thread. */
    private static void ask(Pending p) {
        File root = root();
        if (sNextId == 0) sNextId = lastId(root) + 1;
        p.id = sNextId++;
        sweep(root);
        p.folder = new File(root, Integer.toString(p.id));
        boolean ok = p.folder.mkdirs() || p.folder.isDirectory();
        StringBuilder types = new StringBuilder();
        for (String t : p.types) types.append(t).append('\n');
        if (ok) ok = Native.pick(p.id, p.kind, types.toString(), p.multiple, p.max, p.folder.getPath(), p.durationS, p.quality, startIn(p.types));
        else Log.w(TAG, p.what() + ": its folder could not be made in " + root);
        if (!ok) {
            // As the user backing out, which is what an app hears when no app takes its intent after all.
            Log.i(TAG, p.what() + " not shown: the caller hears RESULT_CANCELED");
            p.status = CANCELED;
            sAnswered.add(p);
            handler().post(Pickers::deliverReady);
            return;
        }
        Log.i(TAG, p.what() + " asked of the shell" + (p.caller != null && p.request >= 0 ? " for " + p.caller.getClass().getName() + ", request code " + p.request : "")
              + (p.output != null ? ", the result to the app's " + p.output.getScheme() + ": URI" : ""));
        sAsked.put(p.id, p);
    }

    /** The shell answered request `id` (AppRunner.deliver, Native.pickAnswer): given to who asked once the app is in front. Main thread. */
    static void answered(int id) {
        String[] a = Native.pickAnswer(id);
        if (a == null || a.length == 0) return;
        Pending p = sAsked.remove(id);
        if (p == null) {
            Log.w(TAG, "an answer to request " + id + ", which nobody here waits for");
            return;
        }
        try { p.status = Integer.parseInt(a[0]); } catch (NumberFormatException e) { p.status = 2; }
        p.names = java.util.Arrays.copyOfRange(a, 1, a.length);
        sAnswered.add(p);
        deliverReady();
    }

    /** The app is in front again (AppRunner.hostPause): the answers that came meanwhile, given. */
    static void resumed() { deliverReady(); }

    /** An activity made again in its place (AppRunner.relaunch: the screen turned under the picker): a result it waits for is the new one's. */
    static void replaced(Activity old, Activity now) {
        for (Pending p : sAsked.values()) if (p.caller == old) p.caller = now;
        for (Pending p : sAnswered) if (p.caller == old) p.caller = now;
    }

    private static void deliverReady() {
        if (sAnswered.isEmpty()) return;
        if (AppRunner.pausedByHost()) {
            if (!sSaidWaiting) Log.i(TAG, "the answer waits for the app to be in front again");
            sSaidWaiting = true;
            return;
        }
        sSaidWaiting = false;
        while (!sAnswered.isEmpty()) {
            Pending p = sAnswered.remove(0);
            try { finish(p); }
            catch (RuntimeException e) {
                // (the app's onActivityResult threw: as on Android, its own exception, which ends it there; said here first)
                Log.e(TAG, p.what() + ": the result's delivery threw", e);
                throw e;
            }
        }
    }

    /* The answer made the app's: URIs, a photo where the app said or a thumbnail, RESULT_OK; or RESULT_CANCELED. */
    private static void finish(final Pending p) {
        if (p.status != OK || p.names.length == 0) {
            removeTree(p.folder);
            if (p.web != null) { Log.i(TAG, p.what() + ": the page's file input hears none"); p.web.onReceiveValue(null); return; }
            result(p, Activity.RESULT_CANCELED, null, p.status == CANCELED ? "the user backed out" : "Windows could not");
            return;
        }
        if (p.web != null) {
            Uri[] uris = uris(p).toArray(new Uri[0]);
            Log.i(TAG, p.what() + ": the page's file input has " + uris.length + " file" + (uris.length == 1 ? "" : "s") + " (" + extensions(p.names) + ")");
            p.web.onReceiveValue(uris);
            return;
        }
        if (p.kind == FILES) {
            ArrayList<Uri> uris = uris(p);
            // As Android's documents UI answers (PickActivity.onTaskFinished): one file as the data, several as the ClipData;
            // read access granted, and with OPEN_DOCUMENT a lasting one (an app takes it with takePersistableUriPermission).
            Intent data = new Intent();
            if (uris.size() == 1) {
                data.setData(uris.get(0));
            } else {
                String[] mimes = new String[uris.size()];
                for (int k = 0; k < mimes.length; k++) mimes[k] = mimeOf(p.names[k]);
                ClipData clip = new ClipData(null, mimes, new ClipData.Item(uris.get(0)));
                for (int k = 1; k < uris.size(); k++) clip.addItem(new ClipData.Item(uris.get(k)));
                data.setClipData(clip);
            }
            data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | (Intent.ACTION_OPEN_DOCUMENT.equals(p.action) ? Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION : 0));
            result(p, Activity.RESULT_OK, data, uris.size() + " file" + (uris.size() == 1 ? "" : "s") + " (" + extensions(p.names) + ")" + (uris.size() > 1 ? " in the ClipData" : ""));
            return;
        }
        final File taken = new File(p.folder, p.names[0]);
        if (p.output != null) {
            // Where the app said, through its own ContentResolver (its FileProvider's openFile) -- off the main thread: a
            // video may be a hundred megabytes.
            new Thread(() -> {
                final String failed = copyTo(taken, p.output);
                handler().post(() -> {
                    removeTree(p.folder);
                    if (failed != null) {
                        Log.w(TAG, p.what() + ": the " + (p.kind == PHOTO ? "photo" : "video") + " could not be written to the app's URI: " + failed);
                        result(p, Activity.RESULT_CANCELED, null, "the copy failed");
                        return;
                    }
                    // Android's camera answers a photo written so with no data, a video with its URI.
                    Intent data = p.kind == VIDEO ? new Intent().setData(p.output).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION) : null;
                    result(p, Activity.RESULT_OK, data, "the " + (p.kind == PHOTO ? "photo" : "video") + " (" + extensions(p.names) + ") written to the app's " + p.output.getScheme() + ": URI");
                });
            }, "megler-camera-copy").start();
            return;
        }
        if (p.kind == PHOTO) {
            Bitmap small = thumbnail(taken);
            removeTree(p.folder);
            if (small == null) {
                result(p, Activity.RESULT_CANCELED, null, "the photo could not be read");
                return;
            }
            // As Android's camera app (CaptureModule): "inline-data", the Bitmap in "data".
            Intent data = new Intent("inline-data").putExtra("data", small);
            result(p, Activity.RESULT_OK, data, "a photo of " + small.getWidth() + "x" + small.getHeight() + " in its data extra");
            return;
        }
        ArrayList<Uri> uris = uris(p);
        result(p, Activity.RESULT_OK, new Intent().setData(uris.get(0)).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "the video (" + extensions(p.names) + ") as its data");
    }

    private static void result(Pending p, int code, Intent data, String what) {
        Activity c = p.caller;
        if (c == null || p.request < 0) {
            Log.i(TAG, p.what() + ": " + what + "; nobody asked for a result");
            return;
        }
        if (c.isFinishing() || c.isDestroyed()) {
            Log.w(TAG, p.what() + ": " + what + "; " + c.getClass().getName() + ", which asked, has gone");
            return;
        }
        Log.i(TAG, p.what() + ": " + (code == Activity.RESULT_OK ? "RESULT_OK, " : "RESULT_CANCELED, ") + what + ", to " + c.getClass().getName() + " (request code " + p.request + ")");
        c.meglerActivityResult(p.request, code, data);
    }

    private static ArrayList<Uri> uris(Pending p) {
        ArrayList<Uri> l = new ArrayList<>();
        for (String n : p.names) l.add(new Uri.Builder().scheme(ContentResolver.SCHEME_CONTENT).authority(AUTHORITY).appendPath(Integer.toString(p.id)).appendPath(n).build());
        return l;
    }

    /* The extensions of the files' names, for the log: never the names. */
    private static String extensions(String[] names) {
        StringBuilder b = new StringBuilder();
        for (String n : names) {
            int dot = n.lastIndexOf('.');
            String e = dot >= 0 && n.length() - dot <= 8 && n.indexOf(' ', dot) < 0 ? n.substring(dot) : "(no extension)";
            if (b.length() > 0) b.append(' ');
            b.append(e);
        }
        return b.toString();
    }

    /* The copy written where the app said (its FileProvider's URI, a file: URI): null when it went, else why not. */
    private static String copyTo(File from, Uri to) {
        ContentResolver resolver = AppRunner.application() != null ? AppRunner.application().getContentResolver() : ContentResolver.self();
        try (InputStream in = new FileInputStream(from);
             ParcelFileDescriptor pfd = resolver.openFileDescriptor(to, "w")) {
            if (pfd == null) return "the app's provider gave nothing to write to";
            try (OutputStream out = new java.io.FileOutputStream(pfd.getFileDescriptor())) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                out.flush();
            }
            return null;
        } catch (Exception e) {
            return e.toString();
        }
    }

    /* A photo made small as Android's camera makes the one it hands back (CameraUtil.makeBitmap): at most 50,000 pixels, a
       sample of the picture by a power of two, then scaled where the decoder took no sample. */
    private static Bitmap thumbnail(File f) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(f.getPath(), bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;
            int sample = 1;
            while ((long) (bounds.outWidth / sample) * (bounds.outHeight / sample) > INLINE_PIXELS) sample *= 2;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inSampleSize = sample;
            Bitmap b = BitmapFactory.decodeFile(f.getPath(), o);
            if (b == null) return null;
            long pixels = (long) b.getWidth() * b.getHeight();
            if (pixels > INLINE_PIXELS) {
                double k = Math.sqrt((double) INLINE_PIXELS / pixels);
                b = Bitmap.createScaledBitmap(b, Math.max(1, (int) (b.getWidth() * k)), Math.max(1, (int) (b.getHeight() * k)), true);
            }
            return b;
        } catch (RuntimeException | OutOfMemoryError e) {
            Log.w(TAG, "a photo's thumbnail: " + e);
            return null;
        }
    }

    // ---- the requests' folders

    /** Where the requests' folders are: the app's data, megler-picked (the files of the app's own, as the shell writes them). */
    static File root() { return new File(Native.dataDir(), "megler-picked"); }

    /* The highest number a request's folder has (a request of an earlier run): the next one is above it. */
    private static int lastId(File root) {
        int last = 0;
        String[] names = root.list();
        if (names != null) for (String n : names) {
            try { last = Math.max(last, Integer.parseInt(n)); } catch (NumberFormatException e) {}
        }
        return last;
    }

    /* The folders of requests older than a week, gone. */
    private static void sweep(File root) {
        File[] dirs = root.listFiles();
        if (dirs == null) return;
        long now = System.currentTimeMillis();
        int gone = 0;
        for (File d : dirs) if (d.isDirectory() && d.lastModified() > 0 && now - d.lastModified() > KEPT_MS && removeTree(d)) gone++;
        if (gone > 0) Log.i(TAG, gone + " folder" + (gone == 1 ? "" : "s") + " of requests older than a week removed");
    }

    private static boolean removeTree(File f) {
        if (f == null) return false;
        File[] inside = f.listFiles();
        if (inside != null) for (File c : inside) removeTree(c);
        return f.delete();
    }

    static String mimeOf(String name) {
        int dot = name.lastIndexOf('.');
        String m = dot >= 0 ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT)) : null;
        return m != null ? m : "application/octet-stream";
    }

    // ---- the provider of content://megler.picked/<request>/<name>

    private static ContentProvider sProvider;
    /** The provider, registered for the app's ContentResolver (AppRunner.run): the files of earlier runs too, for links an app kept. */
    static void install() {
        if (sProvider != null) return;
        sProvider = new Provider();
        sProvider.attachInfo(ContextImpl.app(), null);
        ContentResolver.register(AUTHORITY, sProvider);
    }

    /* The columns a query with no projection has: what an app reads of a picked file under any of Android's pickers. */
    private static final String[] COLUMNS = { "_display_name", "_size", "mime_type", "_data", "document_id", "last_modified", "flags",
        "datetaken", "date_modified", "width", "height", "orientation", "duration_millis" };

    static final class Provider extends ContentProvider {
        @Override public boolean onCreate() { return true; }

        /* The copy a URI names: content://megler.picked/<request number>/<name>, in that request's folder and nowhere else. */
        static File file(Uri u) {
            if (u == null || !AUTHORITY.equals(u.getAuthority())) return null;
            List<String> seg = u.getPathSegments();
            if (seg == null || seg.size() != 2) return null;
            String req = seg.get(0), name = seg.get(1);
            if (req.isEmpty() || !req.chars().allMatch(Character::isDigit)) return null;
            if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return null;
            return new File(new File(root(), req), name);
        }

        @Override
        public Cursor query(Uri u, String[] projection, String selection, String[] args, String sort) {
            File f = file(u);
            String[] asked = projection != null ? projection : COLUMNS;
            // (A column this provider has not is left out, as FileProvider leaves it.)
            ArrayList<String> kept = new ArrayList<>();
            for (String c : asked) if (c != null && known(c) && !kept.contains(c)) kept.add(c);
            MatrixCursor m = new MatrixCursor(kept.toArray(new String[0]));
            if (f == null || !f.isFile()) return m;
            int[] size = null;
            Object[] row = new Object[kept.size()];
            for (int k = 0; k < row.length; k++) {
                String c = kept.get(k);
                if ((c.equals("width") || c.equals("height")) && size == null) size = imageSize(f);
                row[k] = value(u, f, c, size);
            }
            m.addRow(row);
            return m;
        }

        private static boolean known(String c) {
            for (String k : COLUMNS) if (k.equals(c)) return true;
            return c.equals("_id") || c.equals("title") || c.equals("date_added") || c.equals("duration");
        }

        private static Object value(Uri u, File f, String c, int[] size) {
            switch (c) {
            case "_display_name": return f.getName();
            case "title": { String n = f.getName(); int dot = n.lastIndexOf('.'); return dot > 0 ? n.substring(0, dot) : n; }
            case "_size": return f.length();
            case "mime_type": return mimeOf(f.getName());
            case "_data": return f.getPath();
            case "document_id": return u.getPathSegments().get(0) + "/" + f.getName();
            case "_id": return (long) (u.getPathSegments().get(0) + "/" + f.getName()).hashCode() & 0x7fffffffL;
            case "flags": return 0;
            case "last_modified": case "datetaken": return f.lastModified();
            case "date_modified": case "date_added": return f.lastModified() / 1000;
            case "width": return size != null ? size[0] : 0;
            case "height": return size != null ? size[1] : 0;
            case "orientation": return 0;
            case "duration_millis": case "duration": return 0L;
            default: return null;
            }
        }

        /* A picture's size, read from its header (BitmapFactory's bounds); 0 x 0 for anything else. */
        private static int[] imageSize(File f) {
            if (!mimeOf(f.getName()).startsWith("image/")) return new int[] { 0, 0 };
            try {
                BitmapFactory.Options o = new BitmapFactory.Options();
                o.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(f.getPath(), o);
                return new int[] { Math.max(0, o.outWidth), Math.max(0, o.outHeight) };
            } catch (RuntimeException e) {
                return new int[] { 0, 0 };
            }
        }

        @Override
        public String getType(Uri u) {
            File f = file(u);
            return f != null ? mimeOf(f.getName()) : null;
        }

        @Override
        public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
            File f = file(u);
            if (f == null || !f.isFile()) throw new FileNotFoundException("no picked file " + u);
            if (mode != null && (mode.indexOf('w') >= 0 || mode.indexOf('a') >= 0 || mode.indexOf('t') >= 0)) {
                // A copy of what the user picked: a write would change it and not the file the user sees.
                Log.w(TAG, "a picked file opened to write (mode " + mode + "): refused, it is a copy the user never sees");
                throw new FileNotFoundException("a picked file is read only here: " + u);
            }
            return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
        }

        @Override public Uri insert(Uri u, ContentValues v) { return null; }
        @Override public int delete(Uri u, String selection, String[] args) { return 0; }
        @Override public int update(Uri u, ContentValues v, String selection, String[] args) { return 0; }
    }

    // ---- a WebView without a chrome client of the app's

    /**
     * The chrome client a WebView has while the app has given it none (android.webkit.WebView): the page's file input opens
     * the picker (WebChromeClient.onShowFileChooser, as for any client that leaves it), and the rest is what the WebView does
     * with no client at all -- JavaScript's dialogs not shown (alert, confirm, prompt and onbeforeunload answered as cancelled)
     * and the page's question for the location refused -- where a WebChromeClient of the platform's own would show the
     * dialogs and leave the question unanswered.
     */
    public static final class WebDefault extends WebChromeClient {
        @Override public boolean onJsAlert(android.webkit.WebView v, String url, String message, android.webkit.JsResult r) { r.cancel(); return true; }
        @Override public boolean onJsConfirm(android.webkit.WebView v, String url, String message, android.webkit.JsResult r) { r.cancel(); return true; }
        @Override public boolean onJsPrompt(android.webkit.WebView v, String url, String message, String value, android.webkit.JsPromptResult r) { r.cancel(); return true; }
        @Override public boolean onJsBeforeUnload(android.webkit.WebView v, String url, String message, android.webkit.JsResult r) { r.cancel(); return true; }
        @Override public void onGeolocationPermissionsShowPrompt(String origin, android.webkit.GeolocationPermissions.Callback callback) {
            if (callback != null) callback.invoke(origin, false, false);
        }
    }
    private static WebDefault sWebDefault;
    public static WebDefault webDefault() {
        if (sWebDefault == null) sWebDefault = new WebDefault();
        return sWebDefault;
    }
}
