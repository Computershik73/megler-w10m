package android.opengl;

import android.graphics.Bitmap;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Uploading bitmaps as textures, as Android's GLUtils does: in the format and type of the bitmap's config -- ALPHA_8 as
 * GL_ALPHA bytes, RGB_565 as GL_RGB shorts of 5-6-5, ARGB_4444 as GL_RGBA shorts of 4-4-4-4, the others as GL_RGBA bytes,
 * premultiplied, top row first -- or in those the caller names, the rows tight, and the app's unpack alignment put back
 * after.
 *
 * Every bitmap went up as GL_RGBA bytes whatever was asked, and the unpack alignment was left at 1 for good. Google Play
 * services' legacy maps renderer (OpenGL ES 1, Transit's map) puts its labels' pictures, ALPHA_8 bitmaps, into GL_ALPHA
 * textures: OpenGL refused RGBA there, and the street names showed as grey boxes (0.1.0.678).
 */
public final class GLUtils {
    private GLUtils() {}

    private static final int GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A;
    private static final int GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
            GL_UNSIGNED_SHORT_5_6_5 = 0x8363;
    // OpenGL ES 3's sized formats an app may name, and the format of their pixels.
    private static final int GL_RGBA8 = 0x8058, GL_RGB8 = 0x8051, GL_RGB565 = 0x8D62, GL_RGBA4 = 0x8056, GL_RGB5_A1 = 0x8057, GL_SRGB8_ALPHA8 = 0x8C43;

    private static void check(Bitmap b) {
        if (b == null || b.isRecycled()) throw new IllegalArgumentException("bitmap is null or recycled");
    }

    public static int getInternalFormat(Bitmap b) {
        check(b);
        switch (b.getConfig()) {
            case ALPHA_8: return GL_ALPHA;
            case RGB_565: return GL_RGB;
            default: return GL_RGBA;
        }
    }

    public static int getType(Bitmap b) {
        check(b);
        switch (b.getConfig()) {
            case RGB_565: return GL_UNSIGNED_SHORT_5_6_5;
            case ARGB_4444: return GL_UNSIGNED_SHORT_4_4_4_4;
            default: return GL_UNSIGNED_BYTE;
        }
    }

    /* The format of the pixels of an internal format: itself for the unsized ones, the base format of a sized one. */
    private static int pixelFormat(int internalformat) {
        switch (internalformat) {
            case GL_RGBA8: case GL_SRGB8_ALPHA8: case GL_RGBA4: case GL_RGB5_A1: return GL_RGBA;
            case GL_RGB8: case GL_RGB565: return GL_RGB;
            default: return internalformat;
        }
    }

    /*
     * The bitmap's pixels as `format` and `type` ask, from its premultiplied RGBA bytes, top row first, rows tight; null for
     * a pair that is none of these (Android refuses those too).
     */
    private static ByteBuffer pixels(Bitmap b, int format, int type) {
        int n = b.getWidth() * b.getHeight();
        ByteBuffer rgba = ByteBuffer.allocateDirect(n * 4).order(ByteOrder.nativeOrder());
        b.meglerCopyRgba(rgba);
        rgba.position(0);
        if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) return rgba;
        int size;
        if (type == GL_UNSIGNED_BYTE) {
            if (format == GL_ALPHA || format == GL_LUMINANCE) size = 1;
            else if (format == GL_LUMINANCE_ALPHA) size = 2;
            else if (format == GL_RGB) size = 3;
            else return null;
        } else if ((type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB)
                || ((type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) && format == GL_RGBA)) {
            size = 2;
        } else {
            return null;
        }
        byte[] in = new byte[n * 4];
        rgba.get(in);
        byte[] out = new byte[n * size];
        for (int i = 0, o = 0; i < n; i++) {
            int r = in[i * 4] & 255, g = in[i * 4 + 1] & 255, bl = in[i * 4 + 2] & 255, a = in[i * 4 + 3] & 255, s;
            if (type == GL_UNSIGNED_BYTE) {
                if (format == GL_ALPHA) out[o++] = (byte) a;
                else if (format == GL_LUMINANCE) out[o++] = (byte) r;
                else if (format == GL_LUMINANCE_ALPHA) { out[o++] = (byte) r; out[o++] = (byte) a; }
                else { out[o++] = (byte) r; out[o++] = (byte) g; out[o++] = (byte) bl; }
                continue;
            }
            if (type == GL_UNSIGNED_SHORT_5_6_5) s = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3);
            else if (type == GL_UNSIGNED_SHORT_4_4_4_4) s = ((r >> 4) << 12) | ((g >> 4) << 8) | ((bl >> 4) << 4) | (a >> 4);
            else s = ((r >> 3) << 11) | ((g >> 3) << 6) | ((bl >> 3) << 1) | (a >= 128 ? 1 : 0);
            // (Native order, as OpenGL reads a client's shorts: the low byte first on every processor Megler runs on.)
            out[o++] = (byte) s;
            out[o++] = (byte) (s >> 8);
        }
        ByteBuffer buf = ByteBuffer.allocateDirect(out.length).order(ByteOrder.nativeOrder());
        buf.put(out);
        buf.position(0);
        return buf;
    }

    /* The upload itself, the unpack alignment 1 while it lasts and the app's again after. */
    private static void upload(boolean sub, int target, int level, int internalformat, int x, int y, Bitmap b, int format, int type, int border) {
        ByteBuffer px = pixels(b, format, type);
        if (px == null) throw new IllegalArgumentException("invalid Bitmap format");
        int[] was = new int[1];
        GLES20.glGetIntegerv(GLES20.GL_UNPACK_ALIGNMENT, was, 0);
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1);
        try {
            if (sub) GLES20.glTexSubImage2D(target, level, x, y, b.getWidth(), b.getHeight(), format, type, px);
            else GLES20.glTexImage2D(target, level, internalformat, b.getWidth(), b.getHeight(), border, format, type, px);
        } finally {
            if (was[0] > 0 && was[0] != 1) GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, was[0]);
        }
    }

    public static void texImage2D(int target, int level, int internalformat, Bitmap b, int type, int border) {
        check(b);
        if (internalformat < 0) internalformat = getInternalFormat(b);
        if (type < 0) type = getType(b);
        upload(false, target, level, internalformat, 0, 0, b, pixelFormat(internalformat), type, border);
    }

    public static void texImage2D(int target, int level, int internalformat, Bitmap b, int border) {
        check(b);
        texImage2D(target, level, internalformat, b, getType(b), border);
    }

    public static void texImage2D(int target, int level, Bitmap b, int border) {
        texImage2D(target, level, -1, b, -1, border);
    }

    public static void texSubImage2D(int target, int level, int xoffset, int yoffset, Bitmap b) {
        texSubImage2D(target, level, xoffset, yoffset, b, -1, -1);
    }

    public static void texSubImage2D(int target, int level, int xoffset, int yoffset, Bitmap b, int format, int type) {
        check(b);
        if (format < 0) format = getInternalFormat(b);
        if (type < 0) type = getType(b);
        upload(true, target, level, 0, xoffset, yoffset, b, pixelFormat(format), type, 0);
    }

    public static String getEGLErrorString(int error) {
        switch (error) {
        case 0x3000: return "EGL_SUCCESS"; case 0x3001: return "EGL_NOT_INITIALIZED"; case 0x3002: return "EGL_BAD_ACCESS"; case 0x3003: return "EGL_BAD_ALLOC";
        case 0x3004: return "EGL_BAD_ATTRIBUTE"; case 0x3005: return "EGL_BAD_CONFIG"; case 0x3006: return "EGL_BAD_CONTEXT"; case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
        case 0x3008: return "EGL_BAD_DISPLAY"; case 0x3009: return "EGL_BAD_MATCH"; case 0x300A: return "EGL_BAD_NATIVE_PIXMAP"; case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
        case 0x300C: return "EGL_BAD_PARAMETER"; case 0x300D: return "EGL_BAD_SURFACE"; case 0x300E: return "EGL_CONTEXT_LOST";
        default: return "0x" + Integer.toHexString(error);
        }
    }
}
