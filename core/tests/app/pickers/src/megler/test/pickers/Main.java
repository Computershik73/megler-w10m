package megler.test.pickers;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.util.Log;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.util.List;

/**
 * An app's intents for the system's picker and camera, one after another, each answered by the test's stand-in for the shell
 * (core/tests/mg_java_tests.c, system_pick): what it picks for each is known there -- a picture of 640 x 480 (photo.png), a
 * PDF (doc.pdf), the camera's photo (the same picture, as IMG_<time>.jpg), its video (VID_<time>.mp4) -- and the first request
 * hides the app's window while its picker is up, as Windows 10 Mobile's picker does. Each step says how it went; the
 * lifecycle says itself too, so that the test sees the result come after the window came back and before onResume.
 */
public class Main extends Activity {
    private static final String T = "PickersTest";
    private int ok, failed, step = -1;
    private String detail = "";
    private Uri firstPicked;
    private static volatile String sOpenModes = "";
    /* Which Main this is: the first picker turns the screen while it hides the window, and Main is made again (relaunch). */
    private static int sInstances;
    private final int instance = ++sInstances;

    /** What the app's FileProvider was asked to open, in order (Files.openFile). */
    static void heardOpen(String mode) { sOpenModes += mode + ";"; }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView v = new TextView(this);
        v.setText("Pickers");
        setContentView(v);
        if (state != null) {
            // Made again where the screen turned under a picker: the steps go on from the result, which comes to this one.
            step = state.getInt("step");
            ok = state.getInt("ok");
            failed = state.getInt("failed");
            firstPicked = state.getParcelable("first");
            Log.i(T, "onCreate instance=" + instance + " restored step=" + step);
            return;
        }
        Log.i(T, "onCreate");
        v.post(this::next);
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("step", step);
        out.putInt("ok", ok);
        out.putInt("failed", failed);
        out.putParcelable("first", firstPicked);
    }

    @Override protected void onStart() { super.onStart(); Log.i(T, "onStart"); }
    @Override protected void onRestart() { super.onRestart(); Log.i(T, "onRestart"); }
    @Override protected void onResume() { super.onResume(); Log.i(T, "onResume"); }
    @Override protected void onPause() { super.onPause(); Log.i(T, "onPause"); }
    @Override protected void onStop() { super.onStop(); Log.i(T, "onStop"); }
    @Override protected void onDestroy() { super.onDestroy(); Log.i(T, "onDestroy"); }

    private static final String[] STEPS = { "resolve", "get-content", "open-multiple", "pick-images", "pick-media", "cancel", "chooser",
                                            "capture-output", "capture-inline", "video", "web-input", "orientation", "write-refused" };

    private void report(boolean good) {
        if (good) ok++;
        else failed++;
        Log.i(T, STEPS[step] + (good ? " ok" : " FAILED " + detail));
    }

    private boolean fail(String why) {
        detail = why;
        return false;
    }

    /* The next step: each started for a result goes on in onActivityResult, or in the web input's callback. */
    private void next() {
        step++;
        detail = "";
        if (step >= STEPS.length) {
            Log.i(T, "done ok=" + ok + " failed=" + failed);
            return;
        }
        try {
            switch (STEPS[step]) {
            case "resolve": report(resolve()); next(); return;
            case "get-content":
                startActivityForResult(new Intent(Intent.ACTION_GET_CONTENT).setType("image/*").addCategory(Intent.CATEGORY_OPENABLE), 11);
                return;
            case "open-multiple":
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE)
                                       .putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "image/*", "application/pdf" })
                                       .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true), 12);
                return;
            case "pick-images":
                // Android 13's photo picker, as androidx's PickMultipleVisualMedia sends it on Android 14.
                startActivityForResult(new Intent("android.provider.action.PICK_IMAGES").setType("image/*").putExtra("android.provider.extra.PICK_IMAGES_MAX", 5), 13);
                return;
            case "pick-media":
                startActivityForResult(new Intent(Intent.ACTION_PICK, MediaStore.Images.Media.EXTERNAL_CONTENT_URI), 14);
                return;
            case "cancel":
                startActivityForResult(new Intent(Intent.ACTION_GET_CONTENT), 15);
                return;
            case "chooser": {
                Intent pick = new Intent(Intent.ACTION_GET_CONTENT).setType("application/pdf").addCategory(Intent.CATEGORY_OPENABLE);
                Intent chooser = Intent.createChooser(pick, "Choose a PDF");
                // (left out: a camera an app offers beside the documents)
                chooser.putExtra(Intent.EXTRA_INITIAL_INTENTS, new Intent[] { new Intent(MediaStore.ACTION_IMAGE_CAPTURE) });
                startActivityForResult(chooser, 16);
                return;
            }
            case "capture-output": {
                File out = new File(getCacheDir(), "photo-out.jpg");
                out.delete();
                Uri u = new Uri.Builder().scheme("content").authority(Files.AUTHORITY).appendPath("photo-out.jpg").build();
                startActivityForResult(new Intent(MediaStore.ACTION_IMAGE_CAPTURE).putExtra(MediaStore.EXTRA_OUTPUT, u)
                                       .addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION), 17);
                return;
            }
            case "capture-inline":
                startActivityForResult(new Intent(MediaStore.ACTION_IMAGE_CAPTURE), 18);
                return;
            case "video": {
                File out = new File(getCacheDir(), "video-out.mp4");
                out.delete();
                startActivityForResult(new Intent(MediaStore.ACTION_VIDEO_CAPTURE).putExtra(MediaStore.EXTRA_OUTPUT, Uri.fromFile(out))
                                       .putExtra(MediaStore.EXTRA_DURATION_LIMIT, 15).putExtra(MediaStore.EXTRA_VIDEO_QUALITY, 0), 19);
                return;
            }
            case "web-input": webInput(); return;
            case "orientation":
                startActivityForResult(new Intent(Intent.ACTION_GET_CONTENT).setType("image/jpeg"), 20);
                return;
            case "write-refused": report(writeRefused()); next(); return;
            default: next();
            }
        } catch (Throwable e) {
            Log.e(T, STEPS[step] + " threw", e);
            detail = e.toString();
            report(false);
            next();
        }
    }

    @Override
    protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        Log.i(T, "result " + request + " " + code + (data != null && data.getData() != null ? " " + data.getData() : "")
                 + (data != null && data.getClipData() != null ? " clip " + data.getClipData().getItemCount() : "") + " instance=" + instance);
        boolean good;
        try {
            good = check(request, code, data);
        } catch (Throwable e) {
            Log.e(T, STEPS[step] + " threw", e);
            good = fail(e.toString());
        }
        report(good);
        // (the next one once this result's delivery is through, as an app starts its next screen)
        getWindow().getDecorView().post(this::next);
    }

    private boolean check(int request, int code, Intent data) throws Exception {
        switch (request) {
        case 11: {
            if (code != RESULT_OK || data == null || data.getData() == null) return fail("no result");
            firstPicked = data.getData();
            if (!"megler.picked".equals(firstPicked.getAuthority())) return fail("authority " + firstPicked.getAuthority());
            if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0) return fail("no read grant");
            return picture(firstPicked, "photo.png");
        }
        case 12: {
            if (code != RESULT_OK || data == null) return fail("no result");
            ClipData clip = data.getClipData();
            if (data.getData() != null || clip == null || clip.getItemCount() != 2) return fail("not two in the ClipData");
            if ((data.getFlags() & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) == 0) return fail("no lasting grant for OPEN_DOCUMENT");
            getContentResolver().takePersistableUriPermission(clip.getItemAt(1).getUri(), Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return picture(clip.getItemAt(0).getUri(), "photo.png") && pdf(clip.getItemAt(1).getUri());
        }
        case 13: {
            if (code != RESULT_OK || data == null || data.getData() == null) return fail("no result");
            // The photo picker's columns (MediaStore.PickerMediaColumns).
            try (Cursor c = getContentResolver().query(data.getData(), new String[] { "_display_name", "mime_type", "_size", "datetaken", "width", "height", "orientation",
                                                                                     "duration_millis", "_data" }, null, null, null)) {
                if (c == null || !c.moveToFirst()) return fail("no row");
                if (c.getInt(c.getColumnIndexOrThrow("width")) != 640 || c.getInt(c.getColumnIndexOrThrow("height")) != 480) return fail("size " + c.getInt(4) + "x" + c.getInt(5));
                if (c.getLong(c.getColumnIndexOrThrow("datetaken")) <= 0) return fail("datetaken");
                if (!"image/png".equals(c.getString(c.getColumnIndexOrThrow("mime_type")))) return fail("mime_type");
            }
            return picture(data.getData(), "photo.png");
        }
        case 14: {
            if (code != RESULT_OK || data == null || data.getData() == null) return fail("no result");
            // _data, the copy's path, read as a file as old code reads a gallery's picture.
            String path;
            try (Cursor c = getContentResolver().query(data.getData(), new String[] { MediaStore.MediaColumns.DATA }, null, null, null)) {
                if (c == null || !c.moveToFirst()) return fail("no row");
                path = c.getString(0);
            }
            if (path == null || !new File(path).isFile()) return fail("_data " + path);
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, o);
            return o.outWidth == 640 && o.outHeight == 480 || fail("the file at _data is " + o.outWidth + "x" + o.outHeight);
        }
        case 15:
            return code == RESULT_CANCELED && data == null || fail("code " + code);
        case 16:
            if (code != RESULT_OK || data == null || data.getData() == null) return fail("no result");
            return pdf(data.getData());
        case 17: {
            if (code != RESULT_OK) return fail("code " + code);
            // Android's camera answers a photo written to EXTRA_OUTPUT with no data.
            if (data != null) return fail("data " + data);
            File out = new File(getCacheDir(), "photo-out.jpg");
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(out.getPath(), o);
            if (o.outWidth != 640 || o.outHeight != 480) return fail("the photo written is " + o.outWidth + "x" + o.outHeight + ", " + out.length() + " bytes");
            return sOpenModes.contains("w;") || fail("the app's provider was opened " + sOpenModes);
        }
        case 18: {
            if (code != RESULT_OK || data == null) return fail("no result");
            Object b = data.getParcelableExtra("data");
            if (!(b instanceof Bitmap)) return fail("no Bitmap in data");
            Bitmap bm = (Bitmap) b;
            long pixels = (long) bm.getWidth() * bm.getHeight();
            Log.i(T, "the inline photo is " + bm.getWidth() + "x" + bm.getHeight());
            return pixels > 0 && pixels <= 50 * 1024 && Math.abs(bm.getWidth() * 3 - bm.getHeight() * 4) <= 8 || fail(bm.getWidth() + "x" + bm.getHeight());
        }
        case 19: {
            if (code != RESULT_OK || data == null) return fail("no result");
            File out = new File(getCacheDir(), "video-out.mp4");
            if (!Uri.fromFile(out).equals(data.getData())) return fail("data " + data.getData());
            byte[] head = head(new FileInputStream(out), 12);
            return head.length == 12 && new String(head, 4, 8, "US-ASCII").equals("ftypmp42") || fail("the video written: " + out.length() + " bytes");
        }
        case 20: {
            // A camera's JPEG held upright: its Exif says a quarter turn, as the photo picker's orientation column says it.
            if (code != RESULT_OK || data == null || data.getData() == null) return fail("no result");
            try (Cursor c = getContentResolver().query(data.getData(), new String[] { "orientation", "mime_type" }, null, null, null)) {
                if (c == null || !c.moveToFirst()) return fail("no row");
                return c.getInt(0) == 90 && "image/jpeg".equals(c.getString(1)) || fail("orientation " + c.getInt(0) + " " + c.getString(1));
            }
        }
        default:
            return fail("request " + request);
        }
    }

    /* A picked picture: its name and size as OpenableColumns say, its type, and its bytes, which decode to 640 x 480. */
    private boolean picture(Uri u, String name) throws Exception {
        ContentResolver r = getContentResolver();
        long size;
        try (Cursor c = r.query(u, null, null, null, null)) {
            if (c == null || !c.moveToFirst()) return fail("no row for " + u);
            if (!name.equals(c.getString(c.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME)))) return fail("_display_name");
            size = c.getLong(c.getColumnIndexOrThrow(OpenableColumns.SIZE));
        }
        if (!"image/png".equals(r.getType(u))) return fail("getType " + r.getType(u));
        byte[] bytes = head(r.openInputStream(u), Integer.MAX_VALUE);
        if (bytes.length != size) return fail("read " + bytes.length + " of " + size);
        Bitmap b = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
        if (b == null || b.getWidth() != 640 || b.getHeight() != 480) return fail("decoded " + (b == null ? "nothing" : b.getWidth() + "x" + b.getHeight()));
        // and through a descriptor, as Chromium and Glide open one
        try (android.content.res.AssetFileDescriptor fd = r.openAssetFileDescriptor(u, "r")) {
            if (fd == null || head(fd.createInputStream(), 8).length != 8) return fail("openAssetFileDescriptor");
        }
        return true;
    }

    private boolean pdf(Uri u) throws Exception {
        ContentResolver r = getContentResolver();
        if (!"application/pdf".equals(r.getType(u))) return fail("getType " + r.getType(u));
        byte[] head = head(r.openInputStream(u), 5);
        return new String(head, "US-ASCII").equals("%PDF-") || fail("not a PDF");
    }

    private static byte[] head(InputStream in, int most) throws Exception {
        try (InputStream i = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            int n;
            while (out.size() < most && (n = i.read(b, 0, Math.min(b.length, most - out.size()))) > 0) out.write(b, 0, n);
            return out.toByteArray();
        }
    }

    /* What the package manager says of those intents, as an app asks before it offers its buttons. */
    private boolean resolve() {
        PackageManager pm = getPackageManager();
        ResolveInfo files = pm.resolveActivity(new Intent(Intent.ACTION_GET_CONTENT).setType("image/*").addCategory(Intent.CATEGORY_OPENABLE), PackageManager.MATCH_DEFAULT_ONLY);
        if (files == null || !"com.android.documentsui".equals(files.activityInfo.packageName)) return fail("GET_CONTENT " + (files == null ? null : files.activityInfo.packageName));
        if (files.activityInfo.applicationInfo == null || !"com.android.documentsui".equals(files.activityInfo.applicationInfo.packageName)) return fail("its applicationInfo");
        if (!"Files".equals(String.valueOf(files.loadLabel(pm)))) return fail("its label " + files.loadLabel(pm));
        if (new Intent(MediaStore.ACTION_IMAGE_CAPTURE).resolveActivity(pm) == null) return fail("IMAGE_CAPTURE");
        ResolveInfo camera = pm.resolveActivity(new Intent(MediaStore.ACTION_VIDEO_CAPTURE), 0);
        if (camera == null || !"com.android.camera2".equals(camera.activityInfo.packageName)) return fail("VIDEO_CAPTURE");
        if (pm.resolveActivity(new Intent("android.provider.action.PICK_IMAGES"), 0) == null) return fail("PICK_IMAGES");
        if (pm.resolveActivity(new Intent("com.google.android.gms.provider.action.PICK_IMAGES"), PackageManager.MATCH_SYSTEM_ONLY) == null) return fail("Google's PICK_IMAGES");
        List<ResolveInfo> docs = pm.queryIntentActivities(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*"), 0);
        if (docs.size() != 1) return fail("OPEN_DOCUMENT " + docs.size());
        if (!pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)) return fail("no camera");
        // A contact is not the documents UI's.
        if (pm.resolveActivity(new Intent(Intent.ACTION_GET_CONTENT).setType("vnd.android.cursor.item/phone_v2"), 0) != null) return fail("a contact's GET_CONTENT");
        return true;
    }

    /* A WebView's <input type=file multiple accept="image/*"> left to the platform's chrome client, which opens the picker here. */
    private void webInput() {
        WebChromeClient.FileChooserParams params = new WebChromeClient.FileChooserParams() {
            @Override public int getMode() { return MODE_OPEN_MULTIPLE; }
            @Override public String[] getAcceptTypes() { return new String[] { "image/*" }; }
            @Override public boolean isCaptureEnabled() { return false; }
            @Override public CharSequence getTitle() { return null; }
            @Override public String getFilenameHint() { return null; }
            @Override public Intent createIntent() { return null; }
        };
        ValueCallback<Uri[]> callback = uris -> {
            Log.i(T, "web input " + (uris == null ? "none" : uris.length + " " + uris[0]));
            boolean good;
            try {
                good = uris != null && uris.length == 1 && picture(uris[0], "photo.png") || fail("uris");
            } catch (Throwable e) {
                good = fail(e.toString());
            }
            report(good);
            getWindow().getDecorView().post(this::next);
        };
        if (!new WebChromeClient().onShowFileChooser(null, callback, params)) {
            detail = "the platform's chrome client took no file input";
            report(false);
            next();
        }
    }

    /* A picked file is a copy: opened to be written, it is refused. */
    private boolean writeRefused() {
        if (firstPicked == null) return fail("nothing picked");
        try {
            getContentResolver().openOutputStream(firstPicked).close();
            return fail("written");
        } catch (FileNotFoundException e) {
            return true;
        } catch (Exception e) {
            return fail(e.toString());
        }
    }
}
