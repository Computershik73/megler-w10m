// Megler: new in the overlay. What the media provider (megler.MediaProvider) knows of a file without decoding it: its MIME
// type and media type from its name, as Android's MimeUtils and MediaFile have them, and from its first bytes -- and where
// a format keeps them, its end -- the width and height of a picture or a video, the way it is to be turned (EXIF's
// orientation, a video track's matrix), when it was taken (EXIF's DateTimeOriginal, a video's creation time) and how long
// a video or a sound lasts.
//
// Why. Android's media scanner reads these with ExifInterface, BitmapFactory's inJustDecodeBounds and MediaMetadataRetriever
// as it indexes a file. Here BitmapFactory reads a whole file into memory even for its bounds (megler.Gfx.bmDecode takes the
// bytes), ExifInterface and MediaMetadataRetriever are signatures alone, and a phone's gallery has thousands of photos of
// several megabytes each on slow storage. A JPEG's EXIF and its frame header are in its first few kilobytes, a PNG's size
// in its first 24 bytes, an MP4's in its 'moov' box: a few small reads each, never the pixels.
//
// Formats: JPEG (EXIF in APP1, the size from its SOF), PNG (IHDR, and EXIF in an eXIf chunk), WebP (VP8, VP8L, VP8X and its
// EXIF chunk), GIF, BMP, HEIF and AVIF (the first 'ispe' property's size); MP4, 3GP, MOV and M4A (mvhd and the tracks'
// tkhd and hdlr); MP3 (a Xing, Info or VBRI header's frame count, else its bit rate), WAV, FLAC and Ogg (Vorbis, Opus).
// Not here: Matroska and WebM, AVI, WMA and ASF, AMR, MIDI (no size or length: 0); tags (title, artist, album).
package megler;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.HashMap;
import java.util.Locale;

public final class MediaFiles {
    private MediaFiles() {}

    /* MediaStore.Files.FileColumns' media types. */
    public static final int NONE = 0, IMAGE = 1, AUDIO = 2, VIDEO = 3, PLAYLIST = 4, SUBTITLE = 5, DOCUMENT = 6;

    private static final HashMap<String, String> MIME = new HashMap<>();
    private static final HashMap<String, String> EXTENSION = new HashMap<>();
    private static void mime(String type, String... extensions) {
        for (String e : extensions) MIME.put(e, type);
        if (!EXTENSION.containsKey(type)) EXTENSION.put(type, extensions[0]);
    }
    static {
        // The media types of Android's MimeMap (libcore's mime.types and Android's own additions); the first extension
        // is the one a name without one is given (MediaStore's insert).
        mime("image/jpeg", "jpg", "jpeg", "jpe");
        mime("image/png", "png");
        mime("image/gif", "gif");
        mime("image/webp", "webp");
        mime("image/x-ms-bmp", "bmp");
        mime("image/bmp", "bmp");
        mime("image/heic", "heic");
        mime("image/heif", "heif");
        mime("image/avif", "avif");
        mime("image/x-adobe-dng", "dng");
        mime("image/x-canon-cr2", "cr2");
        mime("image/x-nikon-nef", "nef");
        mime("image/x-sony-arw", "arw");
        mime("image/vnd.wap.wbmp", "wbmp");
        mime("image/x-icon", "ico");
        mime("image/svg+xml", "svg");
        mime("video/mp4", "mp4", "m4v");
        mime("video/3gpp", "3gp", "3gpp");
        mime("video/3gpp2", "3g2", "3gpp2");
        mime("video/quicktime", "mov");
        mime("video/x-matroska", "mkv");
        mime("video/webm", "webm");
        mime("video/avi", "avi");
        mime("video/x-ms-wmv", "wmv");
        mime("video/x-ms-asf", "asf");
        mime("video/mp2ts", "ts");
        mime("video/mpeg", "mpeg", "mpg");
        mime("video/x-flv", "flv");
        mime("audio/mpeg", "mp3", "mpga");
        mime("audio/mp4", "m4a", "m4b", "m4p");
        mime("audio/aac", "aac");
        mime("audio/ogg", "ogg", "oga", "opus");
        mime("audio/flac", "flac");
        mime("audio/x-wav", "wav");
        mime("audio/amr", "amr");
        mime("audio/amr-wb", "awb");
        mime("audio/midi", "mid", "midi", "xmf", "rtttl", "imy", "ota");
        mime("audio/x-ms-wma", "wma");
        mime("audio/x-matroska", "mka");
        mime("audio/x-mpegurl", "m3u", "m3u8");
        mime("audio/x-scpls", "pls");
        mime("application/vnd.ms-wpl", "wpl");
        mime("application/x-subrip", "srt");
        mime("text/vtt", "vtt");
        mime("application/pdf", "pdf");
        mime("text/plain", "txt");
        mime("text/html", "html", "htm");
        mime("text/csv", "csv");
        mime("text/xml", "xml");
        mime("application/rtf", "rtf");
        mime("application/msword", "doc");
        mime("application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx");
        mime("application/vnd.ms-excel", "xls");
        mime("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx");
        mime("application/vnd.ms-powerpoint", "ppt");
        mime("application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx");
        mime("application/vnd.oasis.opendocument.text", "odt");
        mime("application/vnd.oasis.opendocument.spreadsheet", "ods");
        mime("application/epub+zip", "epub");
        mime("application/zip", "zip");
        mime("application/x-7z-compressed", "7z");
        mime("application/x-rar-compressed", "rar");
        mime("application/vnd.android.package-archive", "apk");
        mime("application/json", "json");
    }

    /** The MIME type of a file by its name's extension, or null. */
    public static String mimeOf(String name) {
        int dot = name == null ? -1 : name.lastIndexOf('.');
        if (dot < 0 || dot == name.length() - 1) return null;
        return MIME.get(name.substring(dot + 1).toLowerCase(Locale.ROOT));
    }

    /** The extension a file of this MIME type is given (without the dot), or null. */
    public static String extensionOf(String mime) {
        return mime == null ? null : EXTENSION.get(mime.toLowerCase(Locale.ROOT));
    }

    /** MediaStore's media type of a MIME type, as Android's MediaProvider sets it (MimeUtils.resolveMediaType). */
    public static int mediaTypeOf(String mime) {
        if (mime == null) return NONE;
        String m = mime.toLowerCase(Locale.ROOT);
        if (m.equals("audio/x-mpegurl") || m.equals("audio/mpegurl") || m.equals("audio/x-scpls") || m.equals("application/vnd.ms-wpl")
                || m.equals("application/vnd.apple.mpegurl")) return PLAYLIST;
        if (m.startsWith("image/")) return IMAGE;
        if (m.startsWith("video/")) return VIDEO;
        if (m.startsWith("audio/")) return AUDIO;
        if (m.equals("application/x-subrip") || m.equals("text/vtt") || m.equals("application/ttml+xml")) return SUBTITLE;
        if (m.startsWith("text/") || m.equals("application/pdf") || m.equals("application/rtf") || m.startsWith("application/msword")
                || m.startsWith("application/vnd.ms-") || m.startsWith("application/vnd.openxmlformats-officedocument")
                || m.startsWith("application/vnd.oasis.opendocument") || m.equals("application/epub+zip")) return DOCUMENT;
        return NONE;
    }

    /** What the file's own bytes say; each field 0 (or -1 for the times) where it does not. */
    public static final class Meta {
        public int width, height, orientation;
        /* DATE_TAKEN in milliseconds since 1970, -1 where the file does not say; duration in milliseconds, 0 where not. */
        public long dateTaken = -1, duration;
    }

    /**
     * The file's metadata, read from its headers. `lastModified` (milliseconds) is the file's own time, which Android's
     * scanner uses to guess the time zone of an EXIF time that does not say its own. Never throws: a file that cannot be
     * read, or is not what its name says, has what could be read of it.
     */
    public static Meta read(File file, String mime, long lastModified) {
        Meta m = new Meta();
        if (mime == null) return m;
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            String t = mime.toLowerCase(Locale.ROOT);
            switch (t) {
            case "image/jpeg": jpeg(f, m, lastModified); break;
            case "image/png": png(f, m, lastModified); break;
            case "image/webp": webp(f, m, lastModified); break;
            case "image/gif": gif(f, m); break;
            case "image/bmp": case "image/x-ms-bmp": bmp(f, m); break;
            case "image/heic": case "image/heif": case "image/avif": heif(f, m); break;
            case "audio/mpeg": mp3(f, m); break;
            case "audio/x-wav": case "audio/wav": wav(f, m); break;
            case "audio/flac": flac(f, m); break;
            case "audio/ogg": ogg(f, m); break;
            default:
                if (t.startsWith("video/") || t.equals("audio/mp4") || t.equals("audio/3gpp")) mp4(f, m);
                else if (t.startsWith("image/")) sniff(f, m, lastModified);
            }
            // A picture whose bytes are not what its name says (a PNG saved as .jpg, as apps do): read as its bytes say.
            if (m.width == 0 && t.startsWith("image/") && !t.equals("image/*")) sniff(f, m, lastModified);
        } catch (IOException | RuntimeException e) {
            // (What was read stands.)
        }
        return m;
    }

    /* A picture whose name says one format and whose bytes another (a PNG called .jpg): read as its bytes say. */
    private static void sniff(RandomAccessFile f, Meta m, long lastModified) throws IOException {
        byte[] h = new byte[12];
        f.seek(0);
        if (f.read(h) < 12) return;
        f.seek(0);
        if ((h[0] & 255) == 0xFF && (h[1] & 255) == 0xD8) jpeg(f, m, lastModified);
        else if ((h[0] & 255) == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') png(f, m, lastModified);
        else if (h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F' && h[8] == 'W' && h[9] == 'E') webp(f, m, lastModified);
        else if (h[0] == 'G' && h[1] == 'I' && h[2] == 'F') gif(f, m);
        else if (h[0] == 'B' && h[1] == 'M') bmp(f, m);
        else if (h[4] == 'f' && h[5] == 't' && h[6] == 'y' && h[7] == 'p') heif(f, m);
    }

    // ------------------------------------------------------------------------------------------------- reading helpers

    private static byte[] bytes(RandomAccessFile f, long at, int n) throws IOException {
        if (at < 0 || n < 0) return null;
        long left = f.length() - at;
        if (left <= 0) return null;
        byte[] b = new byte[(int) Math.min(n, left)];
        f.seek(at);
        f.readFully(b);
        return b;
    }
    private static int u16be(byte[] b, int i) { return (b[i] & 255) << 8 | (b[i + 1] & 255); }
    private static int u16le(byte[] b, int i) { return (b[i + 1] & 255) << 8 | (b[i] & 255); }
    private static long u32be(byte[] b, int i) { return ((long) (b[i] & 255) << 24) | (b[i + 1] & 255) << 16 | (b[i + 2] & 255) << 8 | (b[i + 3] & 255); }
    private static long u32le(byte[] b, int i) { return ((long) (b[i + 3] & 255) << 24) | (b[i + 2] & 255) << 16 | (b[i + 1] & 255) << 8 | (b[i] & 255); }
    private static long u64be(byte[] b, int i) { return u32be(b, i) << 32 | u32be(b, i + 4); }
    private static boolean is(byte[] b, int i, String s) {
        if (b == null || i < 0 || i + s.length() > b.length) return false;
        for (int k = 0; k < s.length(); k++) if (b[i + k] != (byte) s.charAt(k)) return false;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------- EXIF

    /* EXIF's orientation as the degrees a picture is turned clockwise to be upright, as ExifInterface.getRotationDegrees
       gives them and MediaStore's ORIENTATION has them (the mirrored ones as the turn they include). */
    static int degrees(int exif) {
        switch (exif) {
        case 6: case 7: return 90;
        case 3: case 4: return 180;
        case 8: case 5: return 270;
        default: return 0;
        }
    }

    /**
     * A TIFF structure of EXIF (from its "II" or "MM"), as JPEG's APP1, PNG's eXIf and WebP's EXIF carry it: the orientation
     * of IFD0, and the time the picture was taken from the EXIF IFD (DateTimeOriginal and OffsetTimeOriginal), into `m`.
     */
    static void exif(byte[] t, int base, int len, Meta m, long lastModified) {
        if (t == null || len < 8 || base + len > t.length) return;
        boolean le = t[base] == 'I' && t[base + 1] == 'I';
        if (!le && !(t[base] == 'M' && t[base + 1] == 'M')) return;
        long ifd0 = le ? u32le(t, base + 4) : u32be(t, base + 4);
        String original = null, offset = null;
        long exifIfd = -1;
        for (int pass = 0; pass < 2; pass++) {
            long ifd = pass == 0 ? ifd0 : exifIfd;
            if (ifd < 8 || ifd + 2 > len) continue;
            int at = base + (int) ifd, n = le ? u16le(t, at) : u16be(t, at);
            for (int i = 0; i < n && at + 2 + 12 * (i + 1) <= base + len; i++) {
                int e = at + 2 + 12 * i;
                int tag = le ? u16le(t, e) : u16be(t, e), type = le ? u16le(t, e + 2) : u16be(t, e + 2);
                long count = le ? u32le(t, e + 4) : u32be(t, e + 4);
                if (pass == 0 && tag == 0x0112 && type == 3) m.orientation = degrees(le ? u16le(t, e + 8) : u16be(t, e + 8));
                else if (pass == 0 && tag == 0x8769) exifIfd = le ? u32le(t, e + 8) : u32be(t, e + 8);
                else if (pass == 1 && type == 2 && (tag == 0x9003 || tag == 0x9011) && count > 0 && count < 64) {
                    int from = count <= 4 ? e + 8 : base + (int) (le ? u32le(t, e + 8) : u32be(t, e + 8));
                    if (from < base || from + count > base + len) continue;
                    int k = 0;
                    while (k < count && t[from + k] != 0) k++;
                    String s = new String(t, from, k, java.nio.charset.StandardCharsets.US_ASCII).trim();
                    if (tag == 0x9003) original = s;
                    else offset = s;
                }
            }
        }
        long taken = dateTaken(original, offset, lastModified);
        if (taken > 0) m.dateTaken = taken;
    }

    /**
     * EXIF's "YYYY:MM:DD HH:MM:SS" as Android's media scanner makes DATE_TAKEN of it (ModernMediaScanner.parseOptionalDateTaken):
     * at the offset EXIF gives with it; without one, read as UTC and moved by the difference to the file's own time,
     * rounded to a quarter of an hour, when that is less than a day -- the photo's time zone, guessed. -1 when there is
     * no time, or no guess.
     */
    static long dateTaken(String original, String offset, long lastModified) {
        if (original == null || original.length() < 19) return -1;
        long utc;
        try {
            int y = Integer.parseInt(original.substring(0, 4)), mo = Integer.parseInt(original.substring(5, 7)), d = Integer.parseInt(original.substring(8, 10));
            int h = Integer.parseInt(original.substring(11, 13)), mi = Integer.parseInt(original.substring(14, 16)), s = Integer.parseInt(original.substring(17, 19));
            if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
            java.util.Calendar c = java.util.Calendar.getInstance(java.util.TimeZone.getTimeZone("UTC"), Locale.ROOT);
            c.clear();
            c.set(y, mo - 1, d, h, mi, s);
            utc = c.getTimeInMillis();
        } catch (RuntimeException e) {
            return -1;
        }
        if (offset != null && offset.length() >= 6 && (offset.charAt(0) == '+' || offset.charAt(0) == '-')) {
            try {
                int minutes = Integer.parseInt(offset.substring(1, 3)) * 60 + Integer.parseInt(offset.substring(4, 6));
                return utc - (offset.charAt(0) == '-' ? -minutes : minutes) * 60000L;
            } catch (RuntimeException e) { }
        }
        if (lastModified > 0) {
            long quarter = 15 * 60000L, off = lastModified - utc;
            if (Math.abs(off) < 24 * 3600000L) return utc + Math.round((float) off / quarter) * quarter;
        }
        return -1;
    }

    // ------------------------------------------------------------------------------------------------------- pictures

    /* JPEG: its segments one after another, each read by its length, until the frame's header (SOF0 to SOF15 but DHT, JPG
       and DAC) says the size; APP1's EXIF on the way. */
    private static void jpeg(RandomAccessFile f, Meta m, long lastModified) throws IOException {
        long at = 2, end = f.length();
        byte[] head = bytes(f, 0, 2);
        if (head == null || head.length < 2 || (head[0] & 255) != 0xFF || (head[1] & 255) != 0xD8) return;
        for (int segments = 0; segments < 64 && at + 4 <= end; segments++) {
            byte[] s = bytes(f, at, 4);
            if (s == null || s.length < 4) return;
            if ((s[0] & 255) != 0xFF) return;
            int marker = s[1] & 255;
            if (marker == 0xFF) { at++; continue; }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
            if (marker == 0xD9 || marker == 0xDA) return;
            int len = u16be(s, 2);
            if (len < 2) return;
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                byte[] sof = bytes(f, at + 4, 5);
                if (sof != null && sof.length == 5) {
                    m.height = u16be(sof, 1);
                    m.width = u16be(sof, 3);
                }
                return;
            }
            if (marker == 0xE1 && len > 8) {
                byte[] app = bytes(f, at + 4, len - 2);
                if (app != null && is(app, 0, "Exif\0\0")) exif(app, 6, app.length - 6, m, lastModified);
            }
            at += 2 + len;
        }
    }

    /* PNG: IHDR's size, then the chunks up to the first IDAT (and on to IEND, by their lengths alone) for an eXIf. */
    private static void png(RandomAccessFile f, Meta m, long lastModified) throws IOException {
        byte[] h = bytes(f, 0, 33);
        if (h == null || h.length < 24 || !is(h, 12, "IHDR")) return;
        m.width = (int) u32be(h, 16);
        m.height = (int) u32be(h, 20);
        long at = 8, end = f.length();
        for (int chunks = 0; chunks < 4096 && at + 8 <= end; chunks++) {
            byte[] c = bytes(f, at, 8);
            if (c == null || c.length < 8) return;
            long len = u32be(c, 0);
            if (is(c, 4, "eXIf") && len > 8 && len < (1 << 20)) {
                byte[] x = bytes(f, at + 8, (int) len);
                if (x != null) exif(x, 0, x.length, m, lastModified);
                return;
            }
            if (is(c, 4, "IEND")) return;
            at += 12 + len;
        }
    }

    /* WebP: RIFF's chunks: VP8X's canvas, VP8's or VP8L's frame, EXIF's TIFF. */
    private static void webp(RandomAccessFile f, Meta m, long lastModified) throws IOException {
        byte[] h = bytes(f, 0, 12);
        if (h == null || h.length < 12 || !is(h, 0, "RIFF") || !is(h, 8, "WEBP")) return;
        long at = 12, end = f.length();
        for (int chunks = 0; chunks < 64 && at + 8 <= end; chunks++) {
            byte[] c = bytes(f, at, 8);
            if (c == null || c.length < 8) return;
            long len = u32le(c, 4);
            if (is(c, 0, "VP8X")) {
                byte[] x = bytes(f, at + 8, 10);
                if (x != null && x.length == 10) {
                    m.width = (int) ((x[4] & 255 | (x[5] & 255) << 8 | (x[6] & 255) << 16) + 1);
                    m.height = (int) ((x[7] & 255 | (x[8] & 255) << 8 | (x[9] & 255) << 16) + 1);
                }
            } else if (is(c, 0, "VP8 ") && m.width == 0) {
                byte[] x = bytes(f, at + 8, 10);
                if (x != null && x.length == 10 && (x[3] & 255) == 0x9D && (x[4] & 255) == 0x01 && (x[5] & 255) == 0x2A) {
                    m.width = u16le(x, 6) & 0x3FFF;
                    m.height = u16le(x, 8) & 0x3FFF;
                }
            } else if (is(c, 0, "VP8L") && m.width == 0) {
                byte[] x = bytes(f, at + 8, 5);
                if (x != null && x.length == 5 && (x[0] & 255) == 0x2F) {
                    long bits = u32le(x, 1);
                    m.width = (int) (bits & 0x3FFF) + 1;
                    m.height = (int) ((bits >> 14) & 0x3FFF) + 1;
                }
            } else if (is(c, 0, "EXIF") && len > 8 && len < (1 << 20)) {
                byte[] x = bytes(f, at + 8, (int) len);
                if (x != null) {
                    int base = is(x, 0, "Exif\0\0") ? 6 : 0;
                    exif(x, base, x.length - base, m, lastModified);
                }
            }
            at += 8 + len + (len & 1);
        }
    }

    private static void gif(RandomAccessFile f, Meta m) throws IOException {
        byte[] h = bytes(f, 0, 10);
        if (h == null || h.length < 10 || !is(h, 0, "GIF")) return;
        m.width = u16le(h, 6);
        m.height = u16le(h, 8);
    }

    private static void bmp(RandomAccessFile f, Meta m) throws IOException {
        byte[] h = bytes(f, 0, 26);
        if (h == null || h.length < 26 || !is(h, 0, "BM")) return;
        if (u32le(h, 14) == 12) {
            m.width = u16le(h, 18);
            m.height = u16le(h, 20);
        } else {
            m.width = (int) u32le(h, 18);
            m.height = Math.abs((int) u32le(h, 22));
        }
    }

    /* HEIF and AVIF: the size of the first image spatial extents property ('ispe') in the 'meta' box near the start (the
       primary item's, as a camera writes them); 'irot' turns it, counter-clockwise in quarters. */
    private static void heif(RandomAccessFile f, Meta m) throws IOException {
        byte[] b = bytes(f, 0, 64 * 1024);
        if (b == null) return;
        for (int i = 4; i + 16 <= b.length; i++) {
            if (m.width == 0 && is(b, i, "ispe")) {
                m.width = (int) u32be(b, i + 8);
                m.height = (int) u32be(b, i + 12);
            } else if (is(b, i, "irot") && i + 5 < b.length) {
                m.orientation = ((4 - (b[i + 4] & 3)) % 4) * 90;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------- MP4 and its kind

    /* MP4, 3GP, MOV, M4A: 'moov' found among the top-level boxes (after 'mdat' as often as before it), then 'mvhd' for the
       length and the creation time, and each 'trak' for its size, its matrix and whether it is the video ('hdlr' 'vide'). */
    private static void mp4(RandomAccessFile f, Meta m) throws IOException {
        long at = 0, end = f.length();
        for (int boxes = 0; boxes < 256 && at + 8 <= end; boxes++) {
            byte[] h = bytes(f, at, 16);
            if (h == null || h.length < 8) return;
            long size = u32be(h, 0), head = 8;
            if (size == 1 && h.length >= 16) { size = u64be(h, 8); head = 16; }
            else if (size == 0) size = end - at;
            if (size < head) return;
            if (is(h, 4, "moov")) {
                if (size > 16L << 20) return;
                byte[] moov = bytes(f, at + head, (int) (size - head));
                if (moov != null) moov(moov, 0, moov.length, m);
                return;
            }
            at += size;
        }
    }

    private static void moov(byte[] b, int from, int to, Meta m) {
        for (int at = from; at + 8 <= to; ) {
            long size = u32be(b, at);
            if (size < 8 || at + size > to) return;
            int end = at + (int) size;
            if (is(b, at + 4, "mvhd") && at + 32 <= end) {
                int v = b[at + 8] & 255, p = at + 12;
                long created, scale, duration;
                if (v == 1 && p + 28 <= end) { created = u64be(b, p); scale = u32be(b, p + 16); duration = u64be(b, p + 20); }
                else { created = u32be(b, p); scale = u32be(b, p + 8); duration = u32be(b, p + 12); }
                if (scale > 0 && duration > 0 && duration != 0xFFFFFFFFL) m.duration = duration * 1000 / scale;
                // (Seconds since 1904, UTC; a camera that set no clock writes 0, and some 1970's time from 1904's.)
                long unix = created - 2082844800L;
                if (unix > 31536000L && unix < 4102444800L) m.dateTaken = unix * 1000;
            } else if (is(b, at + 4, "trak")) {
                trak(b, at + 8, end, m);
            }
            at = end;
        }
    }

    private static void trak(byte[] b, int from, int to, Meta m) {
        int tkhd = -1;
        boolean video = false;
        for (int at = from; at + 8 <= to; ) {
            long size = u32be(b, at);
            if (size < 8 || at + size > to) return;
            if (is(b, at + 4, "tkhd")) tkhd = at;
            else if (is(b, at + 4, "mdia")) video = handler(b, at + 8, at + (int) size, "vide");
            at += (int) size;
        }
        if (tkhd < 0 || !video || m.width > 0) return;
        int v = b[tkhd + 8] & 255, p = tkhd + 12 + (v == 1 ? 32 : 20) + 16;
        if (p + 44 > to) return;
        // The matrix's a, b, c, d (16.16): a quarter turn clockwise is (0, 1, -1, 0), as MediaMetadataRetriever's rotation.
        int a = (int) u32be(b, p), bb = (int) u32be(b, p + 4), c = (int) u32be(b, p + 12), d = (int) u32be(b, p + 16);
        if (a == 0 && bb > 0 && c < 0 && d == 0) m.orientation = 90;
        else if (a < 0 && bb == 0 && c == 0 && d < 0) m.orientation = 180;
        else if (a == 0 && bb < 0 && c > 0 && d == 0) m.orientation = 270;
        m.width = (int) (u32be(b, p + 36) >> 16);
        m.height = (int) (u32be(b, p + 40) >> 16);
    }

    private static boolean handler(byte[] b, int from, int to, String type) {
        for (int at = from; at + 8 <= to; ) {
            long size = u32be(b, at);
            if (size < 8 || at + size > to) return false;
            if (is(b, at + 4, "hdlr") && at + 20 <= to) return is(b, at + 16, type);
            at += (int) size;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------------------- sounds

    private static final int[] MP3_RATES_1 = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    private static final int[] MP3_RATES_2 = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    private static final int[] MP3_SAMPLES = { 44100, 48000, 32000, 0 };

    /* MP3: past ID3v2, the first frame's header; a Xing, Info or VBRI header in that frame counts the frames, and without
       one the length is the bytes left at the first frame's bit rate. */
    private static void mp3(RandomAccessFile f, Meta m) throws IOException {
        long start = 0, end = f.length();
        byte[] id3 = bytes(f, 0, 10);
        if (id3 != null && id3.length == 10 && is(id3, 0, "ID3")) {
            start = 10 + ((id3[6] & 127L) << 21 | (id3[7] & 127L) << 14 | (id3[8] & 127L) << 7 | (id3[9] & 127L)) + ((id3[5] & 0x10) != 0 ? 10 : 0);
        }
        byte[] b = bytes(f, start, 64 * 1024);
        if (b == null) return;
        for (int i = 0; i + 4 <= b.length; i++) {
            if ((b[i] & 255) != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
            int version = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, rateIndex = (b[i + 2] >> 4) & 15, sampleIndex = (b[i + 2] >> 2) & 3;
            int mode = (b[i + 3] >> 6) & 3;
            if (version == 1 || layer != 1 || rateIndex == 0 || rateIndex == 15 || sampleIndex == 3) continue;   // Layer III only
            boolean mpeg1 = version == 3;
            int kbps = (mpeg1 ? MP3_RATES_1 : MP3_RATES_2)[rateIndex];
            int rate = MP3_SAMPLES[sampleIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
            int perFrame = mpeg1 ? 1152 : 576;
            int side = mpeg1 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
            int x = i + 4 + side;
            long frames = -1;
            if ((is(b, x, "Xing") || is(b, x, "Info")) && x + 12 <= b.length && (u32be(b, x + 4) & 1) != 0) frames = u32be(b, x + 8);
            else if (is(b, i + 36, "VBRI") && i + 36 + 18 <= b.length) frames = u32be(b, i + 36 + 14);
            if (frames > 0 && rate > 0) m.duration = frames * perFrame * 1000 / rate;
            else if (kbps > 0) m.duration = (end - start - i) * 8 / kbps;
            return;
        }
    }

    /* WAV: 'fmt ''s bytes a second, and the 'data' chunk's length. */
    private static void wav(RandomAccessFile f, Meta m) throws IOException {
        byte[] h = bytes(f, 0, 12);
        if (h == null || h.length < 12 || !is(h, 0, "RIFF") || !is(h, 8, "WAVE")) return;
        long at = 12, end = f.length(), byteRate = 0;
        for (int chunks = 0; chunks < 64 && at + 8 <= end; chunks++) {
            byte[] c = bytes(f, at, 16);
            if (c == null || c.length < 8) return;
            long len = u32le(c, 4);
            if (is(c, 0, "fmt ")) {
                byte[] fmt = bytes(f, at + 8, 16);
                if (fmt != null && fmt.length >= 12) byteRate = u32le(fmt, 8);
            } else if (is(c, 0, "data")) {
                if (byteRate > 0) m.duration = Math.min(len, end - at - 8) * 1000 / byteRate;
                return;
            }
            at += 8 + len + (len & 1);
        }
    }

    /* FLAC: STREAMINFO's sample rate and its count of samples. */
    private static void flac(RandomAccessFile f, Meta m) throws IOException {
        byte[] h = bytes(f, 0, 42);
        if (h == null || h.length < 42 || !is(h, 0, "fLaC") || (h[4] & 127) != 0) return;
        int s = 8;
        long rate = (h[s + 10] & 255L) << 12 | (h[s + 11] & 255L) << 4 | (h[s + 12] & 255L) >> 4;
        long total = (h[s + 13] & 15L) << 32 | u32be(h, s + 14);
        if (rate > 0) m.duration = total * 1000 / rate;
    }

    /* Ogg: the last page's granule position, at the first page's sample rate (Vorbis's own, Opus's always 48 kHz less its
       pre-skip). */
    private static void ogg(RandomAccessFile f, Meta m) throws IOException {
        byte[] first = bytes(f, 0, 128);
        if (first == null || !is(first, 0, "OggS")) return;
        long rate = 0, skip = 0;
        for (int i = 0; i + 20 <= first.length; i++) {
            if (is(first, i, "\u0001vorbis")) { rate = u32le(first, i + 12); break; }
            if (is(first, i, "OpusHead")) { rate = 48000; skip = u16le(first, i + 10); break; }
        }
        if (rate <= 0) return;
        long end = f.length(), from = Math.max(0, end - 65536);
        byte[] tail = bytes(f, from, (int) (end - from));
        if (tail == null) return;
        for (int i = tail.length - 14; i >= 0; i--) {
            if (!is(tail, i, "OggS")) continue;
            long granule = u32le(tail, i + 6) | u32le(tail, i + 10) << 32;
            if (granule > skip) m.duration = (granule - skip) * 1000 / rate;
            return;
        }
    }
}
