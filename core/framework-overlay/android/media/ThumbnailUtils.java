// Overlay of core/framework/gen-src/android/media/ThumbnailUtils.java (tools/build-framework.sh), which had the platform's
// signatures alone: every method answered null. Now as Android's:
//  - createImageThumbnail: the picture decoded subsampled -- the largest power of two that leaves it no smaller than the
//    size asked for in either direction, as BitmapFactory's inSampleSize takes it -- then scaled to fit within that size and
//    turned upright by its EXIF orientation (Android's turns the thumbnail so too: the orientation is not in the JPEG made
//    of it). The size comes from the file's headers (megler.MediaFiles), not from a decode of its bounds, which reads the
//    whole file here. The old form's kinds: MINI_KIND within 512 x 384, MICRO_KIND cropped to 96 x 96.
//  - extractThumbnail: the bitmap scaled to cover the size and its middle cut out, as Android's does.
//  - createVideoThumbnail: a frame as MediaMetadataRetriever gives it, which is nothing here yet, else Windows' own picture of
//    the video where it is in one of the phone's folders (megler.MediaFiles.windowsThumbnail); createAudioThumbnail: the
//    embedded picture as MediaMetadataRetriever gives it. Where there is none: null, or for the forms with a size an
//    IOException, as Android's says it found none.
//
// Why. The media provider (megler.MediaProvider) makes its thumbnails here, as Android's does; and apps call these
// themselves -- extractThumbnail to cut a square avatar or preview, which answered null and was drawn as nothing.
package android.media;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.util.Size;
import java.io.File;
import java.io.IOException;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class ThumbnailUtils {
    public static final int OPTIONS_RECYCLE_INPUT = 2;
    public static final int TARGET_SIZE_MICRO_THUMBNAIL = 96;
    public ThumbnailUtils() {}

    public static Bitmap createAudioThumbnail(File file, Size size, android.os.CancellationSignal signal) throws IOException {
        Bitmap b = embedded(file, size);
        if (b == null) throw new IOException("No album art found in " + file);
        return b;
    }
    public static Bitmap createAudioThumbnail(String filePath, int kind) {
        return embedded(new File(filePath), kindSize(kind));
    }

    /* The picture a file carries in it (an album's cover), as MediaMetadataRetriever finds it. */
    private static Bitmap embedded(File file, Size size) {
        try (MediaMetadataRetriever r = new MediaMetadataRetriever()) {
            r.setDataSource(file.getPath());
            byte[] art = r.getEmbeddedPicture();
            if (art == null) return null;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(art, 0, art.length, o);
            o.inJustDecodeBounds = false;
            o.inSampleSize = sample(o.outWidth, o.outHeight, size);
            Bitmap b = BitmapFactory.decodeByteArray(art, 0, art.length, o);
            return b == null ? null : fit(b, size);
        } catch (RuntimeException e) {
            return null;
        }
    }

    public static Bitmap createImageThumbnail(File file, Size size, android.os.CancellationSignal signal) throws IOException {
        if (file == null || size == null) throw new IllegalArgumentException("file and size are needed");
        if (!file.isFile()) throw new java.io.FileNotFoundException(file.getPath());
        String mime = megler.MediaFiles.mimeOf(file.getName());
        megler.MediaFiles.Meta m = megler.MediaFiles.read(file, mime != null && mime.startsWith("image/") ? mime : "image/*", file.lastModified());
        int w = m.width, h = m.height;
        boolean quarter = m.orientation == 90 || m.orientation == 270;
        // The size asked for is the upright picture's: a picture turned a quarter is decoded to its other way round.
        Size box = quarter ? new Size(size.getHeight(), size.getWidth()) : size;
        BitmapFactory.Options o = new BitmapFactory.Options();
        if (w <= 0 || h <= 0) {
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getPath(), o);
            w = o.outWidth;
            h = o.outHeight;
            o.inJustDecodeBounds = false;
        }
        if (signal != null) signal.throwIfCanceled();
        o.inSampleSize = sample(w, h, box);
        Bitmap b = BitmapFactory.decodeFile(file.getPath(), o);
        if (b == null) throw new IOException("Failed to decode " + file);
        b = fit(b, box);
        if (m.orientation != 0) {
            Matrix turn = new Matrix();
            turn.setRotate(m.orientation, b.getWidth() / 2f, b.getHeight() / 2f);
            Bitmap upright = Bitmap.createBitmap(b, 0, 0, b.getWidth(), b.getHeight(), turn, true);
            if (upright != b) b.recycle();
            b = upright;
        }
        return b;
    }

    public static Bitmap createImageThumbnail(String filePath, int kind) {
        try {
            Bitmap b = createImageThumbnail(new File(filePath), kindSize(kind), null);
            return kind == 3 ? extractThumbnail(b, TARGET_SIZE_MICRO_THUMBNAIL, TARGET_SIZE_MICRO_THUMBNAIL, OPTIONS_RECYCLE_INPUT) : b;
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("ThumbnailUtils", "createImageThumbnail of " + filePath + ": " + e);
            return null;
        }
    }

    public static Bitmap createVideoThumbnail(File file, Size size, android.os.CancellationSignal signal) throws IOException {
        Bitmap b = frame(file, Math.max(size.getWidth(), size.getHeight()));
        if (b == null) throw new IOException("No frame of " + file + " to make a thumbnail of: Windows has none, and MediaMetadataRetriever none here");
        return fit(b, size);
    }
    public static Bitmap createVideoThumbnail(String filePath, int kind) {
        Size s = kindSize(kind);
        Bitmap b = frame(new File(filePath), Math.max(s.getWidth(), s.getHeight()));
        if (b == null) return null;
        b = fit(b, kindSize(kind));
        return kind == 3 ? extractThumbnail(b, TARGET_SIZE_MICRO_THUMBNAIL, TARGET_SIZE_MICRO_THUMBNAIL, OPTIONS_RECYCLE_INPUT) : b;
    }
    /* A frame of the video: MediaMetadataRetriever's, or Windows' own picture of it where the file is in one of the phone's
       folders (megler.MediaFiles.windowsThumbnail), its longer side `size` at most. */
    private static Bitmap frame(File file, int size) {
        try (MediaMetadataRetriever r = new MediaMetadataRetriever()) {
            r.setDataSource(file.getPath());
            Bitmap b = r.getFrameAtTime(-1);
            if (b != null) return b;
        } catch (RuntimeException e) {
        }
        String mime = megler.MediaFiles.mimeOf(file.getName());
        megler.MediaFiles.Meta m = megler.MediaFiles.read(file, mime != null ? mime : "video/mp4", file.lastModified());
        return megler.MediaFiles.windowsThumbnail(file, size, m.orientation, m.width, m.height);
    }

    public static Bitmap extractThumbnail(Bitmap source, int width, int height) { return extractThumbnail(source, width, height, 0); }
    public static Bitmap extractThumbnail(Bitmap source, int width, int height, int options) {
        if (source == null) return null;
        if (width <= 0 || height <= 0) return null;
        // Scaled so that it covers width x height, then its middle cut out of it.
        float scale = Math.max(width / (float) source.getWidth(), height / (float) source.getHeight());
        int sw = Math.max(width, Math.round(source.getWidth() * scale)), sh = Math.max(height, Math.round(source.getHeight() * scale));
        Bitmap scaled = Bitmap.createScaledBitmap(source, sw, sh, true);
        Bitmap out = Bitmap.createBitmap(scaled, (sw - width) / 2, (sh - height) / 2, width, height);
        if (scaled != source && scaled != out) scaled.recycle();
        if ((options & OPTIONS_RECYCLE_INPUT) != 0 && source != out) source.recycle();
        return out;
    }

    /* MediaStore's old kinds: MINI_KIND 512 x 384, MICRO_KIND 96 x 96 (cut out afterwards), FULL_SCREEN_KIND 1024 x 786. */
    private static Size kindSize(int kind) {
        return kind == 3 ? new Size(TARGET_SIZE_MICRO_THUMBNAIL, TARGET_SIZE_MICRO_THUMBNAIL) : kind == 2 ? new Size(1024, 786) : new Size(512, 384);
    }

    /* The largest power of two that leaves w x h no smaller than the box either way (it is scaled down to fit afterwards). */
    private static int sample(int w, int h, Size box) {
        if (w <= 0 || h <= 0 || box == null || box.getWidth() <= 0 || box.getHeight() <= 0) return 1;
        int s = Math.min(w / box.getWidth(), h / box.getHeight());
        return s > 1 ? Integer.highestOneBit(s) : 1;
    }

    /* A bitmap no larger than the box, its proportions kept. */
    private static Bitmap fit(Bitmap b, Size box) {
        if (box == null || b == null) return b;
        float scale = Math.min(box.getWidth() / (float) b.getWidth(), box.getHeight() / (float) b.getHeight());
        if (scale >= 1) return b;
        Bitmap s = Bitmap.createScaledBitmap(b, Math.max(1, Math.round(b.getWidth() * scale)), Math.max(1, Math.round(b.getHeight() * scale)), true);
        if (s != b) b.recycle();
        return s;
    }
}
