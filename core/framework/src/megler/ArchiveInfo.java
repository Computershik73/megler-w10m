package megler;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.XmlBlock;
import android.os.Bundle;
import android.util.TypedValue;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.xmlpull.v1.XmlPullParser;

/** Package metadata of an APK on disk, without changing the running app's manifest. */
public final class ArchiveInfo {
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private ArchiveInfo() {}

    public static PackageInfo read(String path) { return read(path, 0); }

    /**
     * With PackageManager.GET_META_DATA among `flags`, the application's own meta-data too (applicationInfo.metaData), as
     * Android's getPackageArchiveInfo gives it: the meta-data elements directly inside the application element, each value
     * of its type, and null when there is none or the flag is not given. A value that refers to a resource stays the
     * resource's id: the archive's resources are not loaded. (Alibaba's SecurityGuard reads a string of each of its
     * plug-ins' meta-data so: Goofish ended with "BaseBundle.getString on a null object reference".)
     */
    public static PackageInfo read(String path, int flags) {
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
            boolean inApplication = false;
            TypedValue v = new TypedValue();
            while (parser.next() != XmlPullParser.END_DOCUMENT) {
                if (parser.getEventType() != XmlPullParser.START_TAG) continue;
                if (parser.getDepth() == 2) inApplication = "application".equals(parser.getName());
                if ("meta-data".equals(parser.getName())) {
                    String name = parser.getAttributeValue(NS, "name");
                    int vi = parser.indexOf(NS, "value"), ri = parser.indexOf(NS, "resource");
                    if (!inApplication || parser.getDepth() != 3 || (flags & PackageManager.GET_META_DATA) == 0 || name == null) continue;
                    if (app.metaData == null) app.metaData = new Bundle();
                    if (ri >= 0) {
                        app.metaData.putInt(name, parser.getAttributeResourceValue(ri, 0));
                    } else if (vi >= 0) {
                        parser.getAttributeTypedValue(vi, v);
                        if (v.type == TypedValue.TYPE_STRING) app.metaData.putString(name, String.valueOf(v.string));
                        else if (v.type == TypedValue.TYPE_INT_BOOLEAN) app.metaData.putBoolean(name, v.data != 0);
                        else if (v.type == TypedValue.TYPE_FLOAT) app.metaData.putFloat(name, v.getFloat());
                        else app.metaData.putInt(name, v.data);
                    }
                } else if ("manifest".equals(parser.getName())) {
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
