// Overlay of core/framework/src/android/graphics/BitmapFactory.java (tools/build-framework.sh).
// It differs in decodeFile, which caught IOException alone: a null path (Drawable.createFromPath(null), as Sberbank calls it
// from a coroutine) threw a NullPointerException out of it: a fatal exception on the phone (FATAL EXCEPTION, DefaultDispatcher). Android's
// catches any exception there, says it in the log and answers null.
// And in Options.inBitmap, which the original never looked at: a decode now goes into the bitmap given there and hands that
// same bitmap back, as Android's does. Fresco -- the pictures of Facebook's apps, of React Native's Image and of Aliucord's
// Discord -- decodes every still picture into a bitmap of its pool and throws an IllegalStateException when the bitmap that
// comes back is another one: no avatar, photo or preview of theirs was ever shown (Facebook's log: "onProducerFinishWithFailure
// ... stage: DecodeProducer ... java.lang.IllegalStateException"). And a JPEG asked for as RGB_565 is RGB_565, as on Android.
package android.graphics;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

/** Decoding through ImageIO (PNG, JPEG, WebP, GIF, BMP, HEIF). */
public class BitmapFactory {
    public static class Options {
        public boolean inJustDecodeBounds, inMutable = false, inScaled = true, inDither, inPurgeable, inInputShareable, inPreferQualityOverSpeed, inPremultiplied = true;
        public int inSampleSize = 1, inDensity, inTargetDensity, inScreenDensity;
        public Bitmap.Config inPreferredConfig = Bitmap.Config.ARGB_8888;
        public Bitmap inBitmap;
        public byte[] inTempStorage;
        public ColorSpace inPreferredColorSpace;
        public int outWidth, outHeight;
        public String outMimeType;
        public Bitmap.Config outConfig;
        public ColorSpace outColorSpace;
        public boolean mCancel;
        public void requestCancelDecode() { mCancel = true; }
    }
    private static String mime(byte[] d, int o) {
        if (d.length - o > 4 && (d[o] & 255) == 0x89 && d[o + 1] == 'P') return "image/png";
        if (d.length - o > 3 && (d[o] & 255) == 0xFF && (d[o + 1] & 255) == 0xD8) return "image/jpeg";
        if (d.length - o > 12 && d[o] == 'R' && d[o + 8] == 'W') return "image/webp";
        if (d.length - o > 3 && d[o] == 'G' && d[o + 1] == 'I') return "image/gif";
        return "image/png";
    }
    /* The config a decoded picture has: ARGB_8888 (the pixels are kept so whatever the config), but RGB_565 for a JPEG -- which
       has no alpha -- when the app asks for that, as Android's decoder gives it (its bytes then count two a pixel). */
    private static Bitmap.Config configOf(Options o, String mime) {
        return o != null && o.inPreferredConfig == Bitmap.Config.RGB_565 && "image/jpeg".equals(mime) ? Bitmap.Config.RGB_565 : Bitmap.Config.ARGB_8888;
    }
    /* The bitmap a decode goes into (Options.inBitmap), as Android takes it: a recycled one refused, an immutable one passed
       over (said once) for a new bitmap. */
    private static boolean sImmutableSaid;
    private static Bitmap reusable(Options o) {
        Bitmap b = o != null ? o.inBitmap : null;
        if (b == null) return null;
        if (b.isRecycled()) throw new IllegalArgumentException("Cannot reuse a recycled Bitmap");
        if (!b.isMutable()) {
            if (!sImmutableSaid) { sImmutableSaid = true; android.util.Log.w("BitmapFactory", "Unable to reuse an immutable bitmap as an image decoder target."); }
            return null;
        }
        return b;
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length, Options o) {
        if (data == null) return null;
        int[] wh = new int[2];
        int sample = o != null && o.inSampleSize > 1 ? Integer.highestOneBit(o.inSampleSize) : 1;
        String mime = mime(data, offset);
        if (o != null && o.inJustDecodeBounds) {
            megler.Gfx.bmDecode(data, offset, length, sample, wh, true);
            o.outWidth = wh[0]; o.outHeight = wh[1]; o.outMimeType = mime; o.outConfig = configOf(o, mime);
            return null;
        }
        Bitmap reuse = reusable(o);
        long n = megler.Gfx.bmDecode(data, offset, length, sample, wh, false);
        if (n == 0) {
            if (o != null) { o.outWidth = -1; o.outHeight = -1; }
            // (Android's decode methods say so when there was a bitmap to decode into: Fresco catches it and decodes plainly.)
            if (o != null && o.inBitmap != null) throw new IllegalArgumentException("Problem decoding into existing bitmap");
            return null;
        }
        Bitmap.Config config = configOf(o, mime);
        Bitmap b = new Bitmap(n, wh[0], wh[1], config, reuse != null || (o != null && o.inMutable));
        if (config == Bitmap.Config.RGB_565) b.setHasAlpha(false);
        if (o != null) {
            o.outWidth = wh[0]; o.outHeight = wh[1]; o.outMimeType = mime; o.outConfig = config;
            // density scaling, as resources ask for it
            if (o.inScaled && o.inDensity > 0 && o.inTargetDensity > 0 && o.inDensity != o.inTargetDensity) {
                float s = o.inTargetDensity / (float) o.inDensity;
                Bitmap sc = Bitmap.createScaledBitmap(b, Math.max(1, (int) (wh[0] * s + 0.5f)), Math.max(1, (int) (wh[1] * s + 0.5f)), true);
                if (sc != b) { b.recycle(); b = sc; }
                b.setDensity(o.inTargetDensity);
                o.outWidth = b.getWidth(); o.outHeight = b.getHeight();
            }
        }
        if (reuse != null) {
            // Into the bitmap given, which has to hold the picture in what it was made with, as on Android.
            if ((long) b.getByteCount() > reuse.getAllocationByteCount()) {
                b.recycle();
                throw new IllegalArgumentException("Problem decoding into existing bitmap");
            }
            reuse.meglerTakeOver(b);
            return reuse;
        }
        return b;
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length) { return decodeByteArray(data, offset, length, null); }
    public static Bitmap decodeStream(InputStream in, Rect pad, Options o) {
        if (in == null) return null;
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            for (int r; (r = in.read(buf)) > 0; ) out.write(buf, 0, r);
            byte[] d = out.toByteArray();
            return decodeByteArray(d, 0, d.length, o);
        } catch (java.io.IOException e) { return null; }
    }
    public static Bitmap decodeStream(InputStream in) { return decodeStream(in, null, null); }
    public static Bitmap decodeFile(String path, Options o) {
        // As Android's: any failure to open or read the file, a null path included, is a null bitmap and a line in the log.
        try (java.io.FileInputStream in = new java.io.FileInputStream(path)) { return decodeStream(in, null, o); }
        catch (Exception e) { android.util.Log.e("BitmapFactory", "Unable to decode stream: " + e); return null; }
    }
    public static Bitmap decodeFile(String path) { return decodeFile(path, null); }
    public static Bitmap decodeFileDescriptor(java.io.FileDescriptor fd, Rect pad, Options o) {
        try { return decodeStream(new java.io.FileInputStream(fd), pad, o); } catch (Exception e) { return null; }
    }
    public static Bitmap decodeFileDescriptor(java.io.FileDescriptor fd) { return decodeFileDescriptor(fd, null, null); }
    public static Bitmap decodeResource(android.content.res.Resources res, int id, Options o) {
        try (InputStream in = res.openRawResource(id)) {
            if (o == null) o = new Options();
            if (o.inDensity == 0) o.inDensity = res.meglerDrawableDensity(id);
            if (o.inTargetDensity == 0) o.inTargetDensity = res.getDisplayMetrics().densityDpi;
            return decodeStream(in, null, o);
        } catch (Exception e) { return null; }
    }
    public static Bitmap decodeResource(android.content.res.Resources res, int id) { return decodeResource(res, id, null); }
    public static Bitmap decodeResourceStream(android.content.res.Resources res, android.util.TypedValue v, InputStream in, Rect pad, Options o) { return decodeStream(in, pad, o); }
}
