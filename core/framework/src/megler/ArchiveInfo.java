package megler;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.res.XmlBlock;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.xmlpull.v1.XmlPullParser;

/** Package metadata of an APK on disk, without changing the running app's manifest. */
public final class ArchiveInfo {
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private ArchiveInfo() {}

    public static PackageInfo read(String path) {
        if (path == null) return null;
        try (ZipFile zip = new ZipFile(path)) {
            ZipEntry manifest = zip.getEntry("AndroidManifest.xml");
            if (manifest == null || manifest.getSize() > 4 * 1024 * 1024) return null;
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream in = zip.getInputStream(manifest)) {
                byte[] chunk = new byte[8192];
                int n;
                while ((n = in.read(chunk)) != -1) {
                    if (bytes.size() + n > 4 * 1024 * 1024) return null;
                    bytes.write(chunk, 0, n);
                }
            }
            XmlBlock.Parser parser = new XmlBlock(bytes.toByteArray(), "AndroidManifest.xml").newParser();
            PackageInfo info = new PackageInfo();
            ApplicationInfo app = new ApplicationInfo();
            app.sourceDir = app.publicSourceDir = path;
            while (parser.next() != XmlPullParser.END_DOCUMENT) {
                if (parser.getEventType() != XmlPullParser.START_TAG) continue;
                if ("manifest".equals(parser.getName())) {
                    info.packageName = parser.getAttributeValue(null, "package");
                    info.versionCode = parser.getAttributeIntValue(NS, "versionCode", 0);
                    info.versionName = parser.getAttributeValue(NS, "versionName");
                    app.packageName = app.processName = info.packageName;
                } else if ("uses-sdk".equals(parser.getName())) {
                    app.minSdkVersion = parser.getAttributeIntValue(NS, "minSdkVersion", app.minSdkVersion);
                    app.targetSdkVersion = parser.getAttributeIntValue(NS, "targetSdkVersion", app.minSdkVersion);
                }
            }
            if (info.packageName == null || info.packageName.isEmpty()) return null;
            info.applicationInfo = app;
            return info;
        } catch (Exception e) {
            return null;
        }
    }
}
