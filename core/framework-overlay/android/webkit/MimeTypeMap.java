// megler-w10m overlay of Megler/src/java-framework/src/android/webkit/MimeTypeMap.java (tools/build-framework.sh).
// It differs in one method, meglerInstallDefault, which named libcore.content.type.MimeMap: a class of Android 10's libcore
// that the Android 9 one this port runs on does not have. Everything else is the original's, unchanged.
package android.webkit;

import java.util.HashMap;
import java.util.Locale;

/** File extensions to MIME types and back: the common types an app's web content and downloads use. */
public class MimeTypeMap {
    private static final MimeTypeMap sMimeTypeMap = new MimeTypeMap();
    private static final HashMap<String, String> EXT = new HashMap<>(), MIME = new HashMap<>();
    private static void add(String mime, String... exts) {
        for (String e : exts) { if (!EXT.containsKey(e)) EXT.put(e, mime); }
        if (!MIME.containsKey(mime)) MIME.put(mime, exts[0]);
    }
    static {
        add("text/html", "html", "htm", "shtml"); add("text/css", "css"); add("text/javascript", "js", "mjs");
        add("application/javascript", "js"); add("application/json", "json", "map"); add("text/plain", "txt", "text", "log", "ini", "conf");
        add("text/xml", "xml"); add("application/xml", "xml"); add("application/xhtml+xml", "xhtml"); add("text/csv", "csv"); add("text/markdown", "md");
        add("image/png", "png"); add("image/jpeg", "jpg", "jpeg", "jpe"); add("image/gif", "gif"); add("image/webp", "webp"); add("image/svg+xml", "svg", "svgz");
        add("image/bmp", "bmp"); add("image/x-icon", "ico"); add("image/vnd.microsoft.icon", "ico"); add("image/heic", "heic"); add("image/avif", "avif");
        add("audio/mpeg", "mp3"); add("audio/ogg", "ogg", "oga"); add("audio/wav", "wav"); add("audio/x-wav", "wav"); add("audio/aac", "aac"); add("audio/mp4", "m4a");
        add("audio/flac", "flac"); add("audio/midi", "mid", "midi"); add("audio/webm", "weba"); add("audio/opus", "opus");
        add("video/mp4", "mp4", "m4v"); add("video/webm", "webm"); add("video/ogg", "ogv"); add("video/quicktime", "mov"); add("video/3gpp", "3gp"); add("video/x-matroska", "mkv");
        add("font/woff", "woff"); add("font/woff2", "woff2"); add("font/ttf", "ttf"); add("font/otf", "otf"); add("application/vnd.ms-fontobject", "eot");
        add("application/wasm", "wasm"); add("application/pdf", "pdf"); add("application/zip", "zip"); add("application/gzip", "gz");
        add("application/vnd.android.package-archive", "apk"); add("application/octet-stream", "bin");
        add("application/msword", "doc"); add("application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx");
        add("application/vnd.ms-excel", "xls"); add("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx");
        add("application/vnd.ms-powerpoint", "ppt"); add("application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx");
        add("application/rtf", "rtf"); add("application/epub+zip", "epub"); add("application/x-tar", "tar"); add("application/x-7z-compressed", "7z");
        add("application/manifest+json", "webmanifest"); add("text/calendar", "ics"); add("text/vcard", "vcf");
    }
    private MimeTypeMap() {}
    /**
     * The original hands this table to libcore's MimeMap, which Android 10 added. Android 9's libcore has no such class: its
     * libcore.net.MimeUtils keeps a table of its own that nothing can add to, and URLConnection.guessContentTypeFromName
     * reads that. So there is nothing to install here.
     */
    public static void meglerInstallDefault() {}
    public static MimeTypeMap getSingleton() { return sMimeTypeMap; }
    public static String getFileExtensionFromUrl(String url) {
        if (url == null || url.isEmpty()) return "";
        int i = url.lastIndexOf('#'); if (i > 0) url = url.substring(0, i);
        i = url.lastIndexOf('?'); if (i > 0) url = url.substring(0, i);
        i = url.lastIndexOf('/'); String name = i >= 0 ? url.substring(i + 1) : url;
        if (!name.isEmpty() && java.util.regex.Pattern.matches("[a-zA-Z_0-9\\.\\-\\(\\)\\%]+", name)) {
            int dot = name.lastIndexOf('.');
            if (dot >= 0) return name.substring(dot + 1);
        }
        return "";
    }
    public boolean hasMimeType(String mimeType) { return mimeType != null && MIME.containsKey(mimeType.toLowerCase(Locale.ROOT)); }
    public String getMimeTypeFromExtension(String extension) { return extension == null ? null : EXT.get(extension.toLowerCase(Locale.ROOT)); }
    public boolean hasExtension(String extension) { return extension != null && EXT.containsKey(extension.toLowerCase(Locale.ROOT)); }
    public String getExtensionFromMimeType(String mimeType) { return mimeType == null ? null : MIME.get(mimeType.toLowerCase(Locale.ROOT)); }
}
