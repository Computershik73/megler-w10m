package megler.test.data;

import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.database.ContentObserver;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;
import android.util.Size;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Calendar;
import java.util.HashMap;
import java.util.TimeZone;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The phone's media as an app sees them through MediaStore (Megler's media provider, megler.MediaProvider): what the app
 * saves to the gallery, what is found there that it did not save, and how both are queried, read, thumbnailed, watched and
 * deleted. Run twice, on one data folder: in the first run the app has not been let have the storage yet (the step
 * "permissions" asks after these), so it sees only what it saved itself; in the second it has it, and sees everything --
 * what the first run saved under the same ids, with the app its owner still (the app's cache cleared in between).
 *
 * The files are made here, byte by byte, small: a PNG whose eXIf chunk turns it a quarter clockwise and says when it was
 * taken without a time zone, a JPEG of headers alone (the tests' picture decoder reads PNG only) whose EXIF turns it half
 * round and says its time zone, an MP4 of boxes alone (2.5 s, 320 x 240, turned a quarter), a WAV of 1.5 s, a PDF.
 * Each method is a step; what it saw is its detail.
 */
final class MediaSteps {
    static final String PKG = "megler.test.data";
    static final String ROOT = "/storage/emulated/0";
    /* MediaStore's and ContentResolver's names of Android 11, which the SDK the app is built with (29) has not. */
    static final String MATCH_PENDING = "android:query-arg-match-pending", GROUP_BY = "android:query-arg-sql-group-by";
    static final int MATCH_EXCLUDE = 2, MATCH_ONLY = 3;
    static final int RED = 0xFFFF0000, BLUE = 0xFF0000FF;

    private final Context ctx;
    private final ContentResolver cr;
    private final long run;
    String detail;
    /* What this run saved, and when its portrait was taken. */
    Uri portrait, photo, clip, tone, doc;
    long portraitTaken;

    MediaSteps(Context ctx, long run) {
        this.ctx = ctx;
        this.cr = ctx.getContentResolver();
        this.run = run;
    }

    // ------------------------------------------------------------------------------------------------------- the files

    static long utc(int y, int mo, int d, int h, int mi, int s) {
        Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        c.clear();
        c.set(y, mo - 1, d, h, mi, s);
        return c.getTimeInMillis();
    }

    /* EXIF's TIFF, little-endian: IFD0's Orientation and the EXIF IFD's DateTimeOriginal (and OffsetTimeOriginal). */
    static byte[] tiff(int orientation, String original, String offset) {
        ByteBuffer b = ByteBuffer.allocate(256).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) 'I').put((byte) 'I').putShort((short) 42).putInt(8);
        b.putShort((short) 2);
        b.putShort((short) 0x0112).putShort((short) 3).putInt(1).putShort((short) orientation).putShort((short) 0);
        int exifAt = 8 + 2 + 2 * 12 + 4;
        b.putShort((short) 0x8769).putShort((short) 4).putInt(1).putInt(exifAt);
        b.putInt(0);
        int n = offset != null ? 2 : 1, dataAt = exifAt + 2 + 12 * n + 4;
        b.putShort((short) n);
        b.putShort((short) 0x9003).putShort((short) 2).putInt(20).putInt(dataAt);
        if (offset != null) b.putShort((short) 0x9011).putShort((short) 2).putInt(7).putInt(dataAt + 20);
        b.putInt(0);
        b.put(original.getBytes(StandardCharsets.US_ASCII)).put((byte) 0);
        if (offset != null) b.put(offset.getBytes(StandardCharsets.US_ASCII)).put((byte) 0);
        return Arrays.copyOf(b.array(), b.position());
    }

    /* A PNG, its left half one colour and its right half another, with an eXIf chunk after IHDR where `exif` is given. */
    static byte[] png(int w, int h, int left, int right, byte[] exif) throws IOException {
        int[] px = new int[w * h];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = x < w / 2 ? left : right;
        Bitmap b = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        if (!b.compress(Bitmap.CompressFormat.PNG, 100, out)) throw new IOException("no PNG");
        byte[] p = out.toByteArray();
        if (exif == null) return p;
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(p, 0, 33);
        ByteBuffer len = ByteBuffer.allocate(4).putInt(exif.length);
        o.write(len.array());
        byte[] type = "eXIf".getBytes(StandardCharsets.US_ASCII);
        o.write(type);
        o.write(exif);
        java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        crc.update(type);
        crc.update(exif);
        o.write(ByteBuffer.allocate(4).putInt((int) crc.getValue()).array());
        o.write(p, 33, p.length - 33);
        return o.toByteArray();
    }

    /* A JPEG of headers alone: APP1's EXIF and a baseline frame header of w x h. */
    static byte[] jpeg(int w, int h, byte[] tiff) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(0xFF); o.write(0xD8);
        byte[] exif = new byte[6 + tiff.length];
        System.arraycopy("Exif\0\0".getBytes(StandardCharsets.US_ASCII), 0, exif, 0, 6);
        System.arraycopy(tiff, 0, exif, 6, tiff.length);
        o.write(0xFF); o.write(0xE1); o.write((exif.length + 2) >> 8); o.write((exif.length + 2) & 255);
        o.write(exif);
        o.write(new byte[] { (byte) 0xFF, (byte) 0xC0, 0, 17, 8, (byte) (h >> 8), (byte) h, (byte) (w >> 8), (byte) w, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 });
        o.write(0xFF); o.write(0xD9);
        return o.toByteArray();
    }

    static byte[] box(String type, byte[]... parts) {
        int n = 8;
        for (byte[] p : parts) n += p.length;
        ByteBuffer b = ByteBuffer.allocate(n);
        b.putInt(n).put(type.getBytes(StandardCharsets.US_ASCII));
        for (byte[] p : parts) b.put(p);
        return b.array();
    }

    /* An MP4 of boxes alone: mvhd (1000 a second, `ms` long, made at `made`), one video track of w x h turned a quarter. */
    static byte[] mp4(int w, int h, int ms, long made) {
        ByteBuffer mvhd = ByteBuffer.allocate(100);
        mvhd.putInt(0).putInt((int) (made / 1000 + 2082844800L)).putInt((int) (made / 1000 + 2082844800L)).putInt(1000).putInt(ms)
            .putInt(0x00010000).putShort((short) 0x0100).put(new byte[10]);
        int[] identity = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
        for (int v : identity) mvhd.putInt(v);
        mvhd.put(new byte[24]).putInt(2);
        ByteBuffer tkhd = ByteBuffer.allocate(84);
        tkhd.putInt(3).putInt(0).putInt(0).putInt(1).putInt(0).putInt(ms).put(new byte[8]).putShort((short) 0).putShort((short) 0)
            .putShort((short) 0).putShort((short) 0);
        int[] quarter = { 0, 0x10000, 0, -0x10000, 0, 0, 0, 0, 0x40000000 };
        for (int v : quarter) tkhd.putInt(v);
        tkhd.putInt(w << 16).putInt(h << 16);
        ByteBuffer hdlr = ByteBuffer.allocate(25);
        hdlr.putInt(0).putInt(0).put("vide".getBytes(StandardCharsets.US_ASCII)).put(new byte[12]).put((byte) 0);
        byte[] moov = box("moov", box("mvhd", mvhd.array()), box("trak", box("tkhd", tkhd.array()), box("mdia", box("hdlr", hdlr.array()))));
        byte[] ftyp = box("ftyp", "isom\0\0\u0002\0isomiso2mp41".getBytes(StandardCharsets.ISO_8859_1));
        byte[] mdat = box("mdat", new byte[16]);
        ByteBuffer all = ByteBuffer.allocate(ftyp.length + mdat.length + moov.length);
        return all.put(ftyp).put(mdat).put(moov).array();
    }

    /* A WAV of silence: 8000 Hz, one channel, 16 bits, `ms` long. */
    static byte[] wav(int ms) {
        int data = 16 * ms;
        ByteBuffer b = ByteBuffer.allocate(44 + data).order(ByteOrder.LITTLE_ENDIAN);
        b.put("RIFF".getBytes(StandardCharsets.US_ASCII)).putInt(36 + data).put("WAVE".getBytes(StandardCharsets.US_ASCII));
        b.put("fmt ".getBytes(StandardCharsets.US_ASCII)).putInt(16).putShort((short) 1).putShort((short) 1).putInt(8000).putInt(16000)
         .putShort((short) 2).putShort((short) 16);
        b.put("data".getBytes(StandardCharsets.US_ASCII)).putInt(data);
        return b.array();
    }

    static void write(File f, byte[] bytes) throws IOException {
        File parent = f.getParentFile();
        if (parent != null) parent.mkdirs();
        try (FileOutputStream out = new FileOutputStream(f)) { out.write(bytes); }
    }

    static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        in.close();
        return out.toByteArray();
    }

    // ------------------------------------------------------------------------------------------------------- helpers

    /* The rows of a query as maps of column to its text (null as null). */
    ArrayList<HashMap<String, String>> rows(Uri uri, String[] projection, String selection, String[] args, String sort) {
        ArrayList<HashMap<String, String>> l = new ArrayList<>();
        try (Cursor c = cr.query(uri, projection, selection, args, sort)) {
            if (c == null) return null;
            while (c.moveToNext()) {
                HashMap<String, String> m = new HashMap<>();
                for (int i = 0; i < c.getColumnCount(); i++) m.put(c.getColumnName(i), c.isNull(i) ? null : c.getString(i));
                l.add(m);
            }
        }
        return l;
    }
    HashMap<String, String> one(Uri item) {
        ArrayList<HashMap<String, String>> l = rows(item, null, null, null, null);
        return l != null && l.size() == 1 ? l.get(0) : null;
    }
    int count(Uri uri, String selection, String... args) {
        try (Cursor c = cr.query(uri, new String[] { "_id" }, selection, args.length == 0 ? null : args, null)) {
            return c == null ? -1 : c.getCount();
        }
    }
    static void check(StringBuilder wrong, String name, boolean held) { if (!held) wrong.append(wrong.length() == 0 ? "" : " ").append(name); }

    /* A row in short: what the steps look at. */
    static String brief(HashMap<String, String> r) {
        if (r == null) return "null";
        StringBuilder b = new StringBuilder("{");
        for (String c : new String[] { "_id", "_display_name", "relative_path", "mime_type", "media_type", "_size", "width", "height", "orientation",
                                       "datetaken", "duration", "is_pending", "owner_package_name" })
            if (r.containsKey(c)) b.append(b.length() == 1 ? "" : " ").append(c).append('=').append(r.get(c));
        return b.append('}').toString();
    }

    static boolean is(HashMap<String, String> row, String column, Object value) {
        return row != null && String.valueOf(value).equals(row.get(column));
    }

    /* A file saved to the gallery as Android 10 and later have an app do it: inserted pending, written, made visible. */
    Uri save(Uri collection, String name, String mime, String relative, byte[] bytes) throws IOException {
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, mime);
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, relative);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri u = cr.insert(collection, v);
        if (u == null) throw new IOException("insert into " + collection + " gave null");
        try (OutputStream out = cr.openOutputStream(u)) { out.write(bytes); }
        v.clear();
        v.put(MediaStore.MediaColumns.IS_PENDING, 0);
        if (cr.update(u, v, null, null) != 1) throw new IOException("update of " + u + " did not take");
        return u;
    }

    static final String TEST_DIR = "Pictures/Megler Test/";

    // ---------------------------------------------------------------------------------------------------------- steps

    /**
     * media-save: what the app saves to the gallery, as Android 10's way has it (insert pending, write, IS_PENDING=0): the
     * file where RELATIVE_PATH says, under the name given -- in the second run "(1)" after it, the first run's being there
     * --, pending and seen by the app alone until it is written; the run before's still there under its id. And what
     * Android refuses: a folder images may not go in, a MIME type that is no image; a name without an extension given its
     * type's. And MediaStore.Images.Media.insertImage.
     */
    boolean save() throws Exception {
        // Taken now by a camera whose clock is two hours ahead of UTC, and EXIF says no zone: the scanner guesses it from the
        // file's own time (now), and DATE_TAKEN is now. (The layer does not set a file's times: utime is answered and ignored.)
        portraitTaken = System.currentTimeMillis() / 1000 * 1000;
        java.text.SimpleDateFormat exifTime = new java.text.SimpleDateFormat("yyyy:MM:dd HH:mm:ss", java.util.Locale.ROOT);
        exifTime.setTimeZone(TimeZone.getTimeZone("UTC"));
        portrait = save(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "megler-portrait.png", "image/png", TEST_DIR,
                png(40, 30, RED, BLUE, tiff(6, exifTime.format(new java.util.Date(portraitTaken + 2 * 3600000L)), null)));
        photo = save(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "megler-photo", "image/jpeg", TEST_DIR,
                jpeg(64, 48, tiff(3, "2023:01:02 03:04:05", "+03:00")));
        clip = save(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "megler-clip.mp4", "video/mp4", "Movies/Megler Test",
                mp4(320, 240, 2500, utc(2022, 3, 4, 5, 6, 7)));
        tone = save(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, "megler-tone.wav", "audio/x-wav", "Music/Megler Test/", wav(1500));
        doc = save(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "megler-doc.pdf", "application/pdf", "Download/Megler Test/",
                "%PDF-1.4\n%%EOF\n".getBytes(StandardCharsets.US_ASCII));
        HashMap<String, String> p = one(portrait), j = one(photo);
        String suffix = run == 1 ? "" : " (1)";
        boolean named = is(p, "_display_name", "megler-portrait" + suffix + ".png") && is(j, "_display_name", "megler-photo" + suffix + ".jpg")
                && is(p, "_data", ROOT + "/" + TEST_DIR + "megler-portrait" + suffix + ".png") && new File(p.get("_data")).isFile()
                && is(p, "relative_path", TEST_DIR) && is(p, "owner_package_name", PKG) && is(p, "is_pending", 0) && is(p, "title", "megler-portrait" + suffix);

        // Pending: the app's own pending item is seen by it, not where pending ones are left out; seen alone where only they are.
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, "megler-pending.png");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, TEST_DIR);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri pending = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v);
        Bundle exclude = new Bundle(), only = new Bundle();
        exclude.putInt(MATCH_PENDING, MATCH_EXCLUDE);
        only.putInt(MATCH_PENDING, MATCH_ONLY);
        int seen = count(pending, null), excluded, onlyPending;
        try (Cursor c = cr.query(pending, new String[] { "_id" }, exclude, null)) { excluded = c.getCount(); }
        try (Cursor c = cr.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, new String[] { "_id" }, only, null)) { onlyPending = c.getCount(); }
        HashMap<String, String> pr = one(pending);
        boolean pendingOk = seen == 1 && excluded == 0 && onlyPending == 1 && is(pr, "is_pending", 1) && is(pr, "mime_type", "image/png")
                && pr.get("date_expires") != null && cr.delete(pending, null, null) == 1 && count(pending, null) == 0;

        // What Android refuses, and a name given its extension.
        String refused = "";
        v.clear();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, "megler-wrong.png");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, "Music/Megler Test/");
        try { cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v); } catch (IllegalArgumentException e) { refused += "folder;"; }
        v.clear();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, "megler-wrong.txt");
        v.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
        try { cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v); } catch (IllegalArgumentException e) { refused += "type;"; }

        // The first run's portrait, by the id it had, in the second: the same file, the app its owner still.
        File kept = new File(ctx.getFilesDir(), "media-portrait-id.txt");
        boolean keptOk = true;
        String keptSaid = "";
        if (run == 1) write(kept, Long.toString(ContentUris.parseId(portrait)).getBytes(StandardCharsets.US_ASCII));
        else {
            long id = Long.parseLong(new String(read(new java.io.FileInputStream(kept)), StandardCharsets.US_ASCII));
            HashMap<String, String> first = one(ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, id));
            keptSaid = first == null ? "none" : first.get("_display_name") + "/" + first.get("owner_package_name");
            keptOk = is(first, "_display_name", "megler-portrait.png") && is(first, "owner_package_name", PKG);
        }

        // insertImage: a JPEG of the bitmap in Pictures.
        String inserted = MediaStore.Images.Media.insertImage(cr, Bitmap.createBitmap(new int[] { RED, RED, BLUE, BLUE }, 2, 2, Bitmap.Config.ARGB_8888),
                "megler-inserted", "inserted by the data app");
        HashMap<String, String> ins = inserted == null ? null : one(Uri.parse(inserted));
        boolean insertImageOk = is(ins, "relative_path", "Pictures/") && is(ins, "_display_name", "megler-inserted" + suffix + ".jpg")
                && is(ins, "mime_type", "image/jpeg") && is(ins, "is_pending", 0) && Long.parseLong(ins.get("_size")) > 0;

        // A new name moves the file, as DISPLAY_NAME's update does on Android.
        v.clear();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, "megler-tone-moved.wav");
        int moved = cr.update(tone, v, null, null);
        HashMap<String, String> t = one(tone);
        boolean movedOk = moved == 1 && is(t, "_display_name", "megler-tone-moved" + suffix + ".wav") && new File(t.get("_data")).isFile()
                && !new File(ROOT + "/Music/Megler Test/megler-tone" + suffix + ".wav").exists();

        detail = "named " + named + " pending " + seen + "/" + excluded + "/" + onlyPending + " refused " + refused + " kept " + keptSaid
                + " inserted " + brief(ins) + " moved " + brief(t) + " portrait " + brief(p) + " photo " + brief(j);
        return named && pendingOk && refused.equals("folder;type;") && keptOk && insertImageOk && movedOk;
    }

    /**
     * media-gated: what the app may see. A photo in DCIM/Camera that the app did not save (written as a file, as the phone's
     * camera writes one) is not seen in the first run, which has no storage permission, and is in the second, with what
     * Android says of it: its bucket (the folder's path in small letters, hashed as Java hashes a string), its relative
     * path, no owner. Hidden files and a folder with .nomedia are never seen. The app's own are seen in both.
     */
    boolean gated() throws Exception {
        File foreign = new File(ROOT + "/DCIM/Camera/megler-foreign.png");
        if (run == 1) {
            write(foreign, png(16, 16, BLUE, BLUE, null));
            write(new File(ROOT + "/DCIM/Camera/.megler-hidden.png"), png(16, 16, BLUE, BLUE, null));
            write(new File(ROOT + "/Pictures/Megler Hidden/.nomedia"), new byte[0]);
            write(new File(ROOT + "/Pictures/Megler Hidden/megler-unseen.png"), png(16, 16, BLUE, BLUE, null));
        }
        ArrayList<HashMap<String, String>> f = rows(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, null, "_display_name = ?", new String[] { "megler-foreign.png" }, null);
        int hidden = count(MediaStore.Files.getContentUri("external"), "_display_name IN (?, ?)", ".megler-hidden.png", "megler-unseen.png");
        int own = count(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "owner_package_name = ?", PKG);
        boolean seen = f != null && f.size() == (run == 1 ? 0 : 1);
        if (seen && run == 2) {
            HashMap<String, String> r = f.get(0);
            seen = is(r, "relative_path", "DCIM/Camera/") && is(r, "bucket_display_name", "Camera") && r.get("owner_package_name") == null
                    && is(r, "bucket_id", (ROOT + "/DCIM/Camera").toLowerCase(java.util.Locale.ROOT).hashCode()) && is(r, "width", 16)
                    && is(r, "volume_name", "external_primary") && is(r, "_size", foreign.length());
        }
        detail = "foreign " + f + " hidden " + hidden + " own " + own;
        return seen && hidden == 0 && own >= 3 * run;
    }

    /**
     * media-query: the app's SQL against the index -- a selection with arguments, IN, OR over Files' media types, sort
     * orders, LIMIT in the sort order, an item's URI, GROUP BY in a Bundle and the old way inside a selection -- and what
     * the files' headers said of them: sizes, orientation, when they were taken (EXIF's time with its zone; without one,
     * moved to the file's own time's zone), a video's length, size and turn, a sound's length.
     */
    boolean query() throws Exception {
        String own = "owner_package_name = ?";
        Uri images = MediaStore.Images.Media.EXTERNAL_CONTENT_URI, files = MediaStore.Files.getContentUri("external");
        int inFolder = count(images, "relative_path = ? AND " + own, TEST_DIR, PKG);
        int byType = count(images, "mime_type IN (?, ?) AND relative_path = ?", "image/png", "image/jpeg", TEST_DIR);
        int media = count(files, "(media_type = 1 OR media_type = 3) AND " + own + " AND relative_path LIKE '%Megler Test/'", PKG);
        ArrayList<HashMap<String, String>> newest = rows(images, new String[] { "_display_name", "datetaken" }, "relative_path = ?", new String[] { TEST_DIR }, "datetaken DESC LIMIT 1");
        ArrayList<HashMap<String, String>> primary = rows(MediaStore.Images.Media.getContentUri("external_primary"), new String[] { "_id" }, "relative_path = ?", new String[] { TEST_DIR }, null);
        int internal = count(MediaStore.Images.Media.INTERNAL_CONTENT_URI, null);
        HashMap<String, String> p = one(portrait), j = one(photo), v = one(clip), a = one(tone), d = one(doc);
        HashMap<String, String> df = one(MediaStore.Files.getContentUri("external", ContentUris.parseId(doc)));
        // (Each check named, and those that fail said first: the log keeps a line's first kilobyte.)
        StringBuilder wrong = new StringBuilder();
        check(wrong, "portrait-size", is(p, "width", 40) && is(p, "height", 30));
        check(wrong, "portrait-turn", is(p, "orientation", 90));
        check(wrong, "portrait-taken", is(p, "datetaken", portraitTaken));
        check(wrong, "portrait-bucket", is(p, "bucket_id", (ROOT + "/Pictures/Megler Test").toLowerCase(java.util.Locale.ROOT).hashCode()) && is(p, "bucket_display_name", "Megler Test"));
        check(wrong, "portrait-file", is(p, "volume_name", "external_primary") && is(p, "_size", new File(p.get("_data")).length()) && is(p, "resolution", "40\u00d730"));
        check(wrong, "photo", is(j, "width", 64) && is(j, "height", 48) && is(j, "orientation", 180));
        check(wrong, "photo-taken", is(j, "datetaken", utc(2023, 1, 2, 0, 4, 5)));
        check(wrong, "clip", is(v, "duration", 2500) && is(v, "width", 320) && is(v, "height", 240) && is(v, "orientation", 90) && is(v, "mime_type", "video/mp4"));
        check(wrong, "clip-taken", is(v, "datetaken", utc(2022, 3, 4, 5, 6, 7)));
        check(wrong, "tone", is(a, "duration", 1500) && is(a, "is_music", 1) && is(a, "album", "Megler Test") && is(a, "artist", "<unknown>"));
        check(wrong, "doc", is(d, "is_download", 1) && is(d, "mime_type", "application/pdf") && is(df, "media_type", 6));
        boolean meta = wrong.length() == 0;
        // GROUP BY, as an app makes its albums: in a Bundle (Android 11's QUERY_ARG_SQL_GROUP_BY), and inside a selection.
        Bundle q = new Bundle();
        q.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, own);
        q.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, new String[] { PKG });
        q.putString(GROUP_BY, "bucket_id");
        q.putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, "bucket_display_name");
        String[] album = { "bucket_id", "bucket_display_name", "COUNT(*) AS count" };
        HashMap<String, Integer> albums = new HashMap<>();
        try (Cursor c = cr.query(images, album, q, null)) {
            while (c.moveToNext()) albums.put(c.getString(1), c.getInt(2));
        }
        ArrayList<HashMap<String, String>> old = rows(images, album, own + ") GROUP BY (bucket_id", new String[] { PKG }, null);
        boolean grouped = albums.size() == 2 && Integer.valueOf((int) (2 * run)).equals(albums.get("Megler Test")) && Integer.valueOf((int) run).equals(albums.get("Pictures"))
                && old != null && old.size() == 2;
        detail = "wrong [" + wrong + "] folder " + inFolder + " type " + byType + " media " + media + " newest " + newest + " primary " + (primary == null ? -1 : primary.size())
                + " internal " + internal + " albums " + albums + " portrait " + brief(p) + " photo " + brief(j) + " clip " + brief(v) + " tone " + brief(a) + " doc " + brief(df);
        return inFolder == 2 * run && byType == 2 * run && media == 3 * run && newest.size() == 1 && newest.get(0).get("_display_name").startsWith("megler-portrait")
                && primary != null && primary.size() == 2 * run && internal == 0 && meta && grouped;
    }

    /**
     * media-bundle: ContentResolver's query with a Bundle as AndroidX's paging asks it: the selection and its arguments,
     * the sort's columns and direction, LIMIT and OFFSET, and the cursor saying which of them it honoured.
     */
    boolean bundle() {
        Bundle q = new Bundle();
        q.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, "relative_path = ? AND owner_package_name = ?");
        q.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, new String[] { TEST_DIR, PKG });
        q.putStringArray(ContentResolver.QUERY_ARG_SORT_COLUMNS, new String[] { MediaStore.MediaColumns.DISPLAY_NAME });
        q.putInt(ContentResolver.QUERY_ARG_SORT_DIRECTION, ContentResolver.QUERY_SORT_DIRECTION_DESCENDING);
        q.putInt(ContentResolver.QUERY_ARG_LIMIT, 1);
        q.putInt(ContentResolver.QUERY_ARG_OFFSET, 1);
        String name;
        String[] honored;
        int n;
        try (Cursor c = cr.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, new String[] { MediaStore.MediaColumns.DISPLAY_NAME }, q, null)) {
            n = c.getCount();
            name = c.moveToFirst() ? c.getString(0) : null;
            honored = c.getExtras().getStringArray(ContentResolver.EXTRA_HONORED_ARGS);
        }
        // Descending by name: run 1 portrait then photo; run 2 "megler-portrait.png", "megler-portrait (1).png", "megler-photo.jpg", ...
        String want = run == 1 ? "megler-photo.jpg" : "megler-portrait (1).png";
        detail = n + " " + name + " honored " + (honored == null ? null : Arrays.toString(honored));
        return n == 1 && want.equals(name) && honored != null && Arrays.asList(honored).contains(ContentResolver.QUERY_ARG_LIMIT)
                && Arrays.asList(honored).contains(ContentResolver.QUERY_ARG_SORT_COLUMNS);
    }

    /**
     * media-read: an item read as Android gives it -- its stream, its descriptor, an asset descriptor, one of a type it is
     * (and not of one it is not) -- the same bytes as its _data path's, which an app may open itself.
     */
    boolean read() throws Exception {
        HashMap<String, String> p = one(portrait);
        byte[] direct = read(new java.io.FileInputStream(p.get("_data")));
        byte[] stream = read(cr.openInputStream(portrait));
        byte[] byFile = read(cr.openInputStream(Uri.fromFile(new File(p.get("_data")))));
        long fdSize;
        try (android.os.ParcelFileDescriptor fd = cr.openFileDescriptor(portrait, "r")) { fdSize = fd.getStatSize(); }
        byte[] typed;
        try (android.content.res.AssetFileDescriptor afd = cr.openTypedAssetFileDescriptor(portrait, "image/*", null)) { typed = read(afd.createInputStream()); }
        String wrongType = "opened";
        try (android.content.res.AssetFileDescriptor afd = cr.openTypedAssetFileDescriptor(portrait, "video/*", null)) { } catch (java.io.FileNotFoundException e) { wrongType = "refused"; }
        String type = cr.getType(portrait);
        detail = direct.length + " " + stream.length + " " + byFile.length + " " + fdSize + " " + typed.length + " " + wrongType + " " + type;
        return direct.length > 33 && Arrays.equals(direct, stream) && Arrays.equals(direct, byFile) && fdSize == direct.length && Arrays.equals(direct, typed)
                && wrongType.equals("refused") && "image/png".equals(type);
    }

    /**
     * media-thumbnails: ContentResolver.loadThumbnail of the portrait -- upright, its left half (red) now on top --,
     * MediaStore's getThumbnail and queryMiniThumbnail (a JPEG in the app's cache, made then), its MICRO kind cut square,
     * and a picture of a size through openTypedAssetFileDescriptor with EXTRA_SIZE. A video has none here: IOException.
     */
    boolean thumbnails() throws Exception {
        Bitmap t = cr.loadThumbnail(portrait, new Size(20, 20), null);
        boolean upright = t != null && t.getWidth() < t.getHeight() && t.getPixel(t.getWidth() / 2, t.getHeight() / 4) == RED
                && t.getPixel(t.getWidth() / 2, t.getHeight() * 3 / 4) == BLUE;
        long id = ContentUris.parseId(portrait);
        Bitmap mini = MediaStore.Images.Thumbnails.getThumbnail(cr, id, MediaStore.Images.Thumbnails.MINI_KIND, null);
        String path = null, microPath = null;
        try (Cursor c = MediaStore.Images.Thumbnails.queryMiniThumbnail(cr, id, MediaStore.Images.Thumbnails.MINI_KIND, new String[] { "_data", "image_id", "kind" })) {
            if (c.moveToFirst() && c.getLong(1) == id && c.getInt(2) == 1) path = c.getString(0);
        }
        try (Cursor c = MediaStore.Images.Thumbnails.queryMiniThumbnail(cr, id, MediaStore.Images.Thumbnails.MICRO_KIND, new String[] { "_data" })) {
            if (c.moveToFirst()) microPath = c.getString(0);
        }
        Bitmap micro = microPath == null ? null : android.graphics.BitmapFactory.decodeFile(microPath);
        Bundle opts = new Bundle();
        opts.putParcelable(ContentResolver.EXTRA_SIZE, new android.graphics.Point(64, 64));
        long sized;
        try (android.content.res.AssetFileDescriptor afd = cr.openTypedAssetFileDescriptor(portrait, "image/*", opts)) { sized = read(afd.createInputStream()).length; }
        String video = "a thumbnail";
        try { cr.loadThumbnail(clip, new Size(64, 64), null); } catch (IOException e) { video = "IOException"; }
        Bitmap square = android.media.ThumbnailUtils.extractThumbnail(Bitmap.createBitmap(40, 30, Bitmap.Config.ARGB_8888), 10, 10);
        detail = (t == null ? null : t.getWidth() + "x" + t.getHeight()) + " mini " + (mini == null ? null : mini.getWidth() + "x" + mini.getHeight())
                + " path " + path + " micro " + (micro == null ? null : micro.getWidth() + "x" + micro.getHeight()) + " sized " + sized + " video " + video
                + " square " + (square == null ? null : square.getWidth() + "x" + square.getHeight());
        return upright && mini != null && mini.getWidth() == 30 && mini.getHeight() == 40 && path != null && new File(path).length() > 0
                && path.startsWith(ctx.getCacheDir().getPath() + "/megler-media/") && micro != null && micro.getWidth() == 96 && micro.getHeight() == 96
                && sized > 0 && video.equals("IOException") && square != null && square.getWidth() == 10 && square.getHeight() == 10;
    }

    /**
     * media-observer: observers of the media URIs hear the provider's inserts, updates and deletes -- one of the images'
     * collection and what is under it, one of Files' collection alone -- and a cursor of the collection hears them too.
     */
    boolean observer() throws Exception {
        final ArrayList<String> heard = new ArrayList<>(), heardFiles = new ArrayList<>();
        final CountDownLatch[] latch = { new CountDownLatch(1) };
        ContentObserver images = new ContentObserver(null) {
            @Override public void onChange(boolean self, Uri u) {
                synchronized (heard) { heard.add(String.valueOf(u)); }
                latch[0].countDown();
            }
        };
        ContentObserver files = new ContentObserver(null) {
            @Override public void onChange(boolean self, Uri u) { synchronized (heardFiles) { heardFiles.add(String.valueOf(u)); } }
        };
        final CountDownLatch cursorHeard = new CountDownLatch(1);
        Cursor c = cr.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, new String[] { "_id" }, null, null, null);
        c.registerContentObserver(new ContentObserver(null) {
            @Override public void onChange(boolean self) { cursorHeard.countDown(); }
        });
        cr.registerContentObserver(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, images);
        cr.registerContentObserver(MediaStore.Files.getContentUri("external"), false, files);
        try {
            Uri u = save(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "megler-observed.png", "image/png", TEST_DIR, png(4, 4, RED, RED, null));
            boolean inserted = latch[0].await(5, TimeUnit.SECONDS);
            Thread.sleep(300);
            String item = u.toString();
            boolean heardItem;
            synchronized (heard) { heardItem = heard.contains(item); heard.clear(); }
            latch[0] = new CountDownLatch(1);
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.TITLE, "observed");
            boolean updated = cr.update(u, v, null, null) == 1 && latch[0].await(5, TimeUnit.SECONDS);
            latch[0] = new CountDownLatch(1);
            boolean deleted = cr.delete(u, null, null) == 1 && latch[0].await(5, TimeUnit.SECONDS);
            Thread.sleep(300);
            boolean cursor = cursorHeard.await(5, TimeUnit.SECONDS);
            boolean filesHeard;
            synchronized (heardFiles) { filesHeard = heardFiles.contains("content://media/external/file"); }
            detail = "inserted " + inserted + " item " + heardItem + " updated " + updated + " deleted " + deleted + " cursor " + cursor + " files " + heardFiles;
            return inserted && heardItem && updated && deleted && cursor && filesHeard;
        } finally {
            cr.unregisterContentObserver(images);
            cr.unregisterContentObserver(files);
            c.close();
        }
    }

    /**
     * media-scanner: a file the app wrote itself, made known as an app did before Android 10: MediaScannerConnection.scanFile
     * answers with its URI (on another thread), and the old broadcast ACTION_MEDIA_SCANNER_SCAN_FILE indexes one too; a file
     * under .nomedia is none. And a photo that appears with no one saying so (the phone's camera) is found by the next query,
     * the folder's time changed. Each seen where the app may see what it did not save: in the second run.
     */
    boolean scanner() throws Exception {
        final String path = ROOT + "/Pictures/Megler Scan/megler-scanned-" + run + ".png";
        write(new File(path), png(8, 8, RED, BLUE, null));
        final String[] said = new String[2];
        final CountDownLatch done = new CountDownLatch(2);
        android.media.MediaScannerConnection.scanFile(ctx, new String[] { path, ROOT + "/Pictures/Megler Hidden/megler-unseen.png" }, null, (p, u) -> {
            said[p.equals(path) ? 0 : 1] = String.valueOf(u);
            done.countDown();
        });
        boolean answered = done.await(10, TimeUnit.SECONDS);
        boolean uriOk = said[0] != null && said[0].startsWith("content://media/external/images/media/") && "null".equals(said[1]);
        // The broadcast, and a photo no one tells of.
        String broadcastPath = ROOT + "/Pictures/Megler Scan/megler-broadcast-" + run + ".png", latePath = ROOT + "/DCIM/Camera/megler-late-" + run + ".png";
        write(new File(broadcastPath), png(8, 8, RED, BLUE, null));
        ctx.sendBroadcast(new android.content.Intent(android.content.Intent.ACTION_MEDIA_SCANNER_SCAN_FILE, Uri.fromFile(new File(broadcastPath))));
        Thread.sleep(1700);
        write(new File(latePath), png(8, 8, RED, BLUE, null));
        int scanned = 0, broadcast = 0, late = 0;
        for (int i = 0; i < 20; i++) {
            Thread.sleep(250);
            scanned = count(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "_data = ?", path);
            broadcast = count(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "_data = ?", broadcastPath);
            late = count(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "_data = ?", latePath);
            if (run == 1 || (scanned == 1 && broadcast == 1 && late == 1)) break;
        }
        detail = "answered " + answered + " " + Arrays.toString(said) + " scanned " + scanned + " broadcast " + broadcast + " late " + late;
        int want = run == 1 ? 0 : 1;
        return answered && uriOk && scanned == want && broadcast == want && late == want;
    }

    /**
     * media-delete: the app deletes what it saved -- the row and the file go -- and cannot delete what it did not (no
     * WRITE_EXTERNAL_STORAGE): nothing deleted, the file still there.
     */
    boolean delete() throws Exception {
        Uri doomed = save(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "megler-doomed.png", "image/png", TEST_DIR, png(4, 4, BLUE, RED, null));
        HashMap<String, String> j = one(doomed);
        File f = new File(j.get("_data"));
        boolean was = f.isFile();
        int deleted = cr.delete(doomed, null, null);
        boolean gone = was && deleted == 1 && !f.exists() && one(doomed) == null;
        int foreignDeleted = 0;
        File foreign = new File(ROOT + "/DCIM/Camera/megler-foreign.png");
        if (run == 2) {
            ArrayList<HashMap<String, String>> l = rows(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, new String[] { "_id" }, "_data = ?", new String[] { foreign.getPath() }, null);
            if (l != null && l.size() == 1) foreignDeleted = cr.delete(ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, Long.parseLong(l.get(0).get("_id"))), null, null);
            else foreignDeleted = -1;
        }
        detail = "deleted " + deleted + " gone " + gone + " foreign " + foreignDeleted + " still " + foreign.exists();
        return gone && foreignDeleted == 0 && foreign.exists();
    }

}
