// Megler's own android.content.pm.PackageParser: Android's hidden parser of an APK's manifest, under Android 9's names, for
// every app. Android has it on the boot class path, and apps that list what other apps declare make one themselves:
// LibChecker's library index ("lib reference") parses each installed APK with new PackageParser().parsePackage(file, 7)
// where its own manifest reader fails, and the class was not there at all -- its worker thread ended on
// NoClassDefFoundError: android/content/pm/PackageParser (issue #18). Android 9's own class stands on its resources and
// AssetManager natives (ApkAssets, the platform's styleables), which Megler's framework has not: this one reads the APK's
// binary manifest itself, as megler.ArchiveInfo does, into the fields and classes Android's Package, Activity, Service,
// Provider and IntentInfo have. What it does not read: the APK's resources (a label or a <data> that names a string
// resource stays the resource's number), its signatures, its splits.
package android.content.pm;

import android.content.ComponentName;
import android.content.IntentFilter;
import android.content.res.XmlBlock;
import android.os.Bundle;
import android.os.PatternMatcher;
import android.util.TypedValue;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.xmlpull.v1.XmlPullParser;

public class PackageParser {
    public static final String APK_FILE_EXTENSION = ".apk";
    public static final int PARSE_MUST_BE_APK = 1, PARSE_IGNORE_PROCESSES = 2, PARSE_FORWARD_LOCK = 4, PARSE_EXTERNAL_STORAGE = 8,
            PARSE_IS_SYSTEM_DIR = 16, PARSE_COLLECT_CERTIFICATES = 32, PARSE_ENFORCE_CODE = 64, PARSE_FORCE_SDK = 128, PARSE_CHATTY = 0x80000000;
    /*
     * The Android whose apps this parser takes: one asking for a later one (minSdkVersion) is refused, as Android refuses it.
     * Not final, unlike Android's: LibChecker sets it to Integer.MAX_VALUE by reflection at its start ("bypass [PackageParser
     * check]") so as to read the APKs of apps for later Androids, which Android lets it do.
     */
    private static int SDK_VERSION = android.os.Build.VERSION.SDK_INT;
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static final int MAX_MANIFEST = 4 * 1024 * 1024;

    private String[] mSeparateProcesses;
    private boolean mOnlyCoreApps;

    public PackageParser() {}

    public void setSeparateProcesses(String[] procs) { mSeparateProcesses = procs; }
    public void setOnlyCoreApps(boolean onlyCoreApps) { mOnlyCoreApps = onlyCoreApps; }
    public void setDisplayMetrics(android.util.DisplayMetrics metrics) {}
    public void setCacheDir(File cacheDir) {}

    public static boolean isApkPath(String path) { return path != null && path.endsWith(APK_FILE_EXTENSION); }
    public static final boolean isApkFile(File file) { return file != null && isApkPath(file.getName()); }

    public Package parsePackage(File packageFile, int flags, boolean useCaches) throws PackageParserException { return parsePackage(packageFile, flags); }

    /** An APK, or a folder of an installed app (its base.apk), as Android 9's parsePackage. */
    public Package parsePackage(File packageFile, int flags) throws PackageParserException {
        if (packageFile != null && packageFile.isDirectory()) {
            if ((flags & PARSE_MUST_BE_APK) != 0) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_NOT_APK, "Not an APK: " + packageFile);
            File base = new File(packageFile, "base.apk");
            if (!base.isFile()) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_NOT_APK, "No packages found in split");
            Package p = parseMonolithicPackage(base, flags);
            p.codePath = packageFile.getPath();
            p.applicationInfo.setCodePath(p.codePath);
            return p;
        }
        return parseMonolithicPackage(packageFile, flags);
    }

    public Package parseMonolithicPackage(File apkFile, int flags) throws PackageParserException {
        String path = apkFile == null ? null : apkFile.getPath();
        byte[] manifest = readManifest(apkFile);
        Package pkg;
        try {
            pkg = parseManifest(manifest, path, flags);
        } catch (PackageParserException e) {
            throw e;
        } catch (Exception e) {
            throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_UNEXPECTED_EXCEPTION, "Failed to read manifest from " + path, e);
        }
        if (mOnlyCoreApps && !pkg.coreApp) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_MANIFEST_MALFORMED, "Not a coreApp: " + path);
        pkg.codePath = pkg.baseCodePath = path;
        return pkg;
    }

    /* The APK's AndroidManifest.xml, its bytes; refused as Android refuses an APK it cannot open or that has none. */
    private static byte[] readManifest(File apkFile) throws PackageParserException {
        String path = apkFile == null ? null : apkFile.getPath();
        if (apkFile == null || !apkFile.isFile()) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_NOT_APK, "Failed to parse " + path);
        try (ZipFile zip = new ZipFile(apkFile)) {
            ZipEntry entry = zip.getEntry("AndroidManifest.xml");
            if (entry == null) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_BAD_MANIFEST, "Failed to parse " + path + ": no AndroidManifest.xml");
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream in = zip.getInputStream(entry)) {
                byte[] chunk = new byte[8192];
                for (int n; (n = in.read(chunk)) != -1;) {
                    if (bytes.size() + n > MAX_MANIFEST) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_BAD_MANIFEST, "Failed to parse " + path + ": its manifest is too large");
                    bytes.write(chunk, 0, n);
                }
            }
            return bytes.toByteArray();
        } catch (java.io.IOException e) {
            throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_NOT_APK, "Failed to parse " + path, e);
        }
    }

    /* The manifest, as Android 9's parseBaseApk reads what is here: the package, the SDKs, permissions, the application and its components. */
    private Package parseManifest(byte[] manifest, String path, int flags) throws Exception {
        XmlBlock.Parser p = new XmlBlock(manifest, "AndroidManifest.xml").newParser();
        Package pkg = null;
        ApplicationInfo app = null;
        Component<?> component = null;
        IntentInfo filter = null;
        boolean inApplication = false;
        int minSdk = 1, targetSdk = 0;
        TypedValue v = new TypedValue();
        for (int t; (t = p.next()) != XmlPullParser.END_DOCUMENT;) {
            if (t == XmlPullParser.END_TAG) {
                String name = p.getName();
                if ("intent-filter".equals(name)) filter = null;
                else if ("application".equals(name)) inApplication = false;
                else if (component != null && component.tag.equals(name)) {
                    finish(component, targetSdk);
                    component = null;
                }
                continue;
            }
            if (t != XmlPullParser.START_TAG) continue;
            String tag = p.getName();
            if (pkg == null) {
                if (!"manifest".equals(tag)) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_MANIFEST_MALFORMED, "No <manifest> tag in " + path);
                String name = p.getAttributeValue(null, "package");
                if (name == null || name.isEmpty()) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_BAD_PACKAGE_NAME, "<manifest> has no package in " + path);
                pkg = new Package(name);
                app = pkg.applicationInfo;
                app.sourceDir = app.publicSourceDir = app.scanSourceDir = app.scanPublicSourceDir = path;
                pkg.mVersionCode = app.versionCode = p.getAttributeIntValue(NS, "versionCode", 0);
                pkg.mVersionCodeMajor = p.getAttributeIntValue(NS, "versionCodeMajor", 0);
                pkg.mVersionName = p.getAttributeValue(NS, "versionName");
                pkg.mSharedUserId = p.getAttributeValue(NS, "sharedUserId");
                pkg.mSharedUserLabel = p.getAttributeResourceValue(NS, "sharedUserLabel", 0);
                pkg.installLocation = p.getAttributeIntValue(NS, "installLocation", -1);
                pkg.coreApp = p.getAttributeBooleanValue(null, "coreApp", false);
                pkg.mCompileSdkVersion = p.getAttributeIntValue(NS, "compileSdkVersion", 0);
                pkg.mCompileSdkVersionCodename = p.getAttributeValue(NS, "compileSdkVersionCodename");
                continue;
            }
            switch (tag) {
            case "uses-sdk": {
                minSdk = p.getAttributeIntValue(NS, "minSdkVersion", 1);
                targetSdk = p.getAttributeIntValue(NS, "targetSdkVersion", minSdk);
                if (minSdk > SDK_VERSION)
                    throw new PackageParserException(PackageManager.INSTALL_FAILED_OLDER_SDK, "Requires newer sdk version #" + minSdk + " (current version is #" + SDK_VERSION + ")");
                break;
            }
            case "uses-permission": case "uses-permission-sdk-23": case "uses-permission-sdk-m": {
                String name = p.getAttributeValue(NS, "name");
                int max = p.getAttributeIntValue(NS, "maxSdkVersion", 0);
                if (name != null && (max <= 0 || max >= SDK_VERSION) && !pkg.requestedPermissions.contains(name)) pkg.requestedPermissions.add(name.intern());
                break;
            }
            case "permission": {
                Permission perm = new Permission(pkg);
                item(p, perm.info, pkg.packageName);
                perm.info.group = p.getAttributeValue(NS, "permissionGroup");
                perm.info.protectionLevel = p.getAttributeIntValue(NS, "protectionLevel", PermissionInfo.PROTECTION_NORMAL);
                pkg.permissions.add(perm);
                break;
            }
            case "permission-group": {
                PermissionGroup group = new PermissionGroup(pkg);
                item(p, group.info, pkg.packageName);
                pkg.permissionGroups.add(group);
                break;
            }
            case "instrumentation": {
                Instrumentation in = new Instrumentation(pkg);
                item(p, in.info, pkg.packageName);
                in.info.targetPackage = p.getAttributeValue(NS, "targetPackage");
                in.info.handleProfiling = p.getAttributeBooleanValue(NS, "handleProfiling", false);
                in.info.functionalTest = p.getAttributeBooleanValue(NS, "functionalTest", false);
                in.info.sourceDir = in.info.publicSourceDir = path;
                pkg.instrumentation.add(in);
                break;
            }
            case "uses-library":
                if (inApplication) {
                    String name = p.getAttributeValue(NS, "name");
                    if (name != null) (p.getAttributeBooleanValue(NS, "required", true) ? pkg.usesLibraries : pkg.usesOptionalLibraries).add(name);
                }
                break;
            case "uses-feature": {
                FeatureInfo f = new FeatureInfo();
                f.name = p.getAttributeValue(NS, "name");
                f.reqGlEsVersion = p.getAttributeIntValue(NS, "glEsVersion", 0);
                if (p.getAttributeBooleanValue(NS, "required", true)) f.flags |= FeatureInfo.FLAG_REQUIRED;
                if (pkg.reqFeatures == null) pkg.reqFeatures = new ArrayList<>();
                pkg.reqFeatures.add(f);
                break;
            }
            case "protected-broadcast": {
                String name = p.getAttributeValue(NS, "name");
                if (name != null) {
                    if (pkg.protectedBroadcasts == null) pkg.protectedBroadcasts = new ArrayList<>();
                    pkg.protectedBroadcasts.add(name);
                }
                break;
            }
            case "application": {
                inApplication = true;
                app.className = full(p.getAttributeValue(NS, "name"), pkg.packageName);
                app.name = app.className;
                app.labelRes = p.getAttributeResourceValue(NS, "label", 0);
                if (app.labelRes == 0) app.nonLocalizedLabel = p.getAttributeValue(NS, "label");
                app.icon = p.getAttributeResourceValue(NS, "icon", 0);
                app.logo = p.getAttributeResourceValue(NS, "logo", 0);
                app.banner = p.getAttributeResourceValue(NS, "banner", 0);
                app.theme = p.getAttributeResourceValue(NS, "theme", 0);
                app.descriptionRes = p.getAttributeResourceValue(NS, "description", 0);
                app.permission = p.getAttributeValue(NS, "permission");
                app.taskAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), pkg.packageName, pkg.packageName);
                String process = p.getAttributeValue(NS, "process");
                app.processName = (flags & PARSE_IGNORE_PROCESSES) != 0 || process == null || process.isEmpty() ? pkg.packageName
                        : process.startsWith(":") ? pkg.packageName + process : process;
                app.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                app.appComponentFactory = full(p.getAttributeValue(NS, "appComponentFactory"), pkg.packageName);
                app.backupAgentName = full(p.getAttributeValue(NS, "backupAgent"), pkg.packageName);
                app.manageSpaceActivityName = full(p.getAttributeValue(NS, "manageSpaceActivity"), pkg.packageName);
                int f = 0;
                if (p.getAttributeBooleanValue(NS, "hasCode", true)) f |= ApplicationInfo.FLAG_HAS_CODE;
                if (p.getAttributeBooleanValue(NS, "allowBackup", true)) f |= ApplicationInfo.FLAG_ALLOW_BACKUP;
                if (p.getAttributeBooleanValue(NS, "allowClearUserData", true)) f |= ApplicationInfo.FLAG_ALLOW_CLEAR_USER_DATA;
                if (p.getAttributeBooleanValue(NS, "debuggable", false)) f |= ApplicationInfo.FLAG_DEBUGGABLE;
                if (p.getAttributeBooleanValue(NS, "largeHeap", false)) f |= ApplicationInfo.FLAG_LARGE_HEAP;
                if (p.getAttributeBooleanValue(NS, "persistent", false)) f |= ApplicationInfo.FLAG_PERSISTENT;
                if (p.getAttributeBooleanValue(NS, "testOnly", false)) f |= ApplicationInfo.FLAG_TEST_ONLY;
                if (p.getAttributeBooleanValue(NS, "supportsRtl", false)) f |= ApplicationInfo.FLAG_SUPPORTS_RTL;
                if (p.getAttributeBooleanValue(NS, "extractNativeLibs", true)) f |= ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS;
                if (p.getAttributeBooleanValue(NS, "usesCleartextTraffic", targetSdk < 28)) f |= ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC;
                if (p.getAttributeBooleanValue(NS, "hardwareAccelerated", targetSdk >= 14)) f |= ApplicationInfo.FLAG_HARDWARE_ACCELERATED;
                pkg.baseHardwareAccelerated = (f & ApplicationInfo.FLAG_HARDWARE_ACCELERATED) != 0;
                if (p.getAttributeBooleanValue(NS, "isGame", false)) f |= ApplicationInfo.FLAG_IS_GAME;
                app.flags |= f;
                break;
            }
            case "activity": case "activity-alias": case "receiver": case "service": case "provider": {
                if (!inApplication) break;
                component = component(p, tag, pkg, app, flags);
                break;
            }
            case "intent-filter": {
                if (component == null) break;
                filter = component.newIntent();
                filter.setPriority(p.getAttributeIntValue(NS, "priority", 0));
                filter.setAutoVerify(p.getAttributeBooleanValue(NS, "autoVerify", false));
                filter.labelRes = p.getAttributeResourceValue(NS, "label", 0);
                if (filter.labelRes == 0) filter.nonLocalizedLabel = p.getAttributeValue(NS, "label");
                filter.icon = p.getAttributeResourceValue(NS, "icon", 0);
                filter.logo = p.getAttributeResourceValue(NS, "logo", 0);
                filter.banner = p.getAttributeResourceValue(NS, "banner", 0);
                break;
            }
            case "action":
                if (filter != null) {
                    String name = p.getAttributeValue(NS, "name");
                    if (name != null) filter.addAction(name);
                }
                break;
            case "category":
                if (filter != null) {
                    String name = p.getAttributeValue(NS, "name");
                    if (name != null) {
                        filter.addCategory(name);
                        if (android.content.Intent.CATEGORY_DEFAULT.equals(name)) filter.hasDefault = true;
                    }
                }
                break;
            case "data":
                if (filter != null) data(p, filter);
                break;
            case "meta-data": {
                Bundle into;
                if (component != null) into = component.metaData != null ? component.metaData : (component.metaData = new Bundle());
                else if (inApplication) into = pkg.mAppMetaData != null ? pkg.mAppMetaData : (pkg.mAppMetaData = new Bundle());
                else break;
                metaData(p, into, v);
                if (component != null) component.info().metaData = component.metaData;
                break;
            }
            }
        }
        if (pkg == null) throw new PackageParserException(PackageManager.INSTALL_PARSE_FAILED_MANIFEST_EMPTY, "Empty manifest in " + path);
        app.minSdkVersion = minSdk;
        app.targetSdkVersion = targetSdk > 0 ? targetSdk : minSdk;
        // (The providers' default, exported before Android 4.2, goes by the target that is known only now.)
        for (Provider pr : pkg.providers) if (!pr.exportedSet) pr.info.exported = app.targetSdkVersion < 17;
        return pkg;
    }

    /* A component's attributes, as Android 9's parseActivity, parseService and parseProvider read them. */
    private Component<?> component(XmlBlock.Parser p, String tag, Package pkg, ApplicationInfo app, int flags) {
        String name = full(p.getAttributeValue(NS, "name"), pkg.packageName);
        ComponentInfo info;
        Component<?> c;
        switch (tag) {
        case "service": {
            ServiceInfo s = new ServiceInfo();
            s.permission = p.getAttributeValue(NS, "permission");
            if (s.permission == null) s.permission = app.permission;
            if (p.getAttributeBooleanValue(NS, "stopWithTask", false)) s.flags |= ServiceInfo.FLAG_STOP_WITH_TASK;
            if (p.getAttributeBooleanValue(NS, "isolatedProcess", false)) s.flags |= ServiceInfo.FLAG_ISOLATED_PROCESS;
            Service sv = new Service(pkg, name, s);
            pkg.services.add(sv);
            info = s;
            c = sv;
            break;
        }
        case "provider": {
            ProviderInfo pi = new ProviderInfo();
            pi.authority = p.getAttributeValue(NS, "authorities");
            String permission = p.getAttributeValue(NS, "permission");
            pi.readPermission = p.getAttributeValue(NS, "readPermission");
            pi.writePermission = p.getAttributeValue(NS, "writePermission");
            if (pi.readPermission == null) pi.readPermission = permission != null ? permission : app.permission;
            if (pi.writePermission == null) pi.writePermission = permission != null ? permission : app.permission;
            pi.grantUriPermissions = p.getAttributeBooleanValue(NS, "grantUriPermissions", false);
            pi.multiprocess = p.getAttributeBooleanValue(NS, "multiprocess", false);
            pi.initOrder = p.getAttributeIntValue(NS, "initOrder", 0);
            Provider pr = new Provider(pkg, name, pi);
            pr.syncable = p.getAttributeBooleanValue(NS, "syncable", false);
            pkg.providers.add(pr);
            info = pi;
            c = pr;
            break;
        }
        default: {
            ActivityInfo a = new ActivityInfo();
            if ("activity-alias".equals(tag)) {
                a.targetActivity = full(p.getAttributeValue(NS, "targetActivity"), pkg.packageName);
                for (Activity target : pkg.activities) {
                    if (!target.info.name.equals(a.targetActivity)) continue;
                    a.theme = target.info.theme;
                    a.launchMode = target.info.launchMode;
                    a.screenOrientation = target.info.screenOrientation;
                    a.configChanges = target.info.configChanges;
                    a.softInputMode = target.info.softInputMode;
                    a.flags = target.info.flags;
                    a.taskAffinity = target.info.taskAffinity;
                    a.processName = target.info.processName;
                }
            } else {
                a.taskAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), app.taskAffinity, pkg.packageName);
            }
            a.theme = p.getAttributeResourceValue(NS, "theme", a.theme);
            a.launchMode = p.getAttributeIntValue(NS, "launchMode", a.launchMode);
            a.screenOrientation = p.getAttributeIntValue(NS, "screenOrientation", a.screenOrientation);
            a.configChanges = p.getAttributeIntValue(NS, "configChanges", a.configChanges);
            a.softInputMode = p.getAttributeIntValue(NS, "windowSoftInputMode", a.softInputMode);
            a.uiOptions = p.getAttributeIntValue(NS, "uiOptions", app.uiOptions);
            a.permission = p.getAttributeValue(NS, "permission");
            if (a.permission == null) a.permission = app.permission;
            a.parentActivityName = full(p.getAttributeValue(NS, "parentActivityName"), pkg.packageName);
            if (p.getAttributeBooleanValue(NS, "excludeFromRecents", false)) a.flags |= ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS;
            if (p.getAttributeBooleanValue(NS, "noHistory", false)) a.flags |= ActivityInfo.FLAG_NO_HISTORY;
            if (p.getAttributeBooleanValue(NS, "hardwareAccelerated", pkg.baseHardwareAccelerated)) a.flags |= ActivityInfo.FLAG_HARDWARE_ACCELERATED;
            Activity ac = new Activity(pkg, name, a, tag);
            ("receiver".equals(tag) ? pkg.receivers : pkg.activities).add(ac);
            info = a;
            c = ac;
            break;
        }
        }
        info.name = name;
        info.packageName = pkg.packageName;
        info.applicationInfo = app;
        info.labelRes = p.getAttributeResourceValue(NS, "label", 0);
        if (info.labelRes == 0) info.nonLocalizedLabel = p.getAttributeValue(NS, "label");
        info.icon = p.getAttributeResourceValue(NS, "icon", 0);
        info.logo = p.getAttributeResourceValue(NS, "logo", 0);
        info.banner = p.getAttributeResourceValue(NS, "banner", 0);
        info.descriptionRes = p.getAttributeResourceValue(NS, "description", 0);
        info.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
        info.directBootAware = p.getAttributeBooleanValue(NS, "directBootAware", false);
        info.splitName = p.getAttributeValue(NS, "splitName");
        String process = p.getAttributeValue(NS, "process");
        if (info.processName == null || process != null)
            info.processName = (flags & PARSE_IGNORE_PROCESSES) != 0 || process == null || process.isEmpty() ? app.processName
                    : process.startsWith(":") ? pkg.packageName + process : process;
        c.exportedSet = p.indexOf(NS, "exported") >= 0;
        info.exported = p.getAttributeBooleanValue(NS, "exported", false);
        c.tag = tag;
        return c;
    }

    /* What Android decides once a component's filters are read: one exported by none is exported when it has a filter (a provider by its target). */
    private static void finish(Component<?> c, int targetSdk) {
        if (!c.exportedSet && !(c instanceof Provider) && c.info() instanceof ComponentInfo) ((ComponentInfo) c.info()).exported = !c.intents.isEmpty();
    }

    /* A <data> of a filter, as Android's parseIntent adds it. Its values as they are: the APK's string resources are not read. */
    private static void data(XmlBlock.Parser p, IntentInfo f) throws IntentFilter.MalformedMimeTypeException {
        String type = p.getAttributeValue(NS, "mimeType");
        if (type != null) f.addDataType(type);
        String scheme = p.getAttributeValue(NS, "scheme");
        if (scheme != null) f.addDataScheme(scheme);
        String[] ssps = { "ssp", "sspPrefix", "sspPattern" };
        int[] kinds = { PatternMatcher.PATTERN_LITERAL, PatternMatcher.PATTERN_PREFIX, PatternMatcher.PATTERN_SIMPLE_GLOB };
        for (int k = 0; k < ssps.length; k++) {
            String s = p.getAttributeValue(NS, ssps[k]);
            if (s != null) f.addDataSchemeSpecificPart(s, kinds[k]);
        }
        String host = p.getAttributeValue(NS, "host");
        if (host != null) f.addDataAuthority(host, p.getAttributeValue(NS, "port"));
        String[] paths = { "path", "pathPrefix", "pathPattern" };
        for (int k = 0; k < paths.length; k++) {
            String s = p.getAttributeValue(NS, paths[k]);
            if (s != null) f.addDataPath(s, kinds[k]);
        }
    }

    /* A <meta-data>: its value of its type, a resource as its number (android:resource, or a reference the APK's resources would resolve). */
    private static void metaData(XmlBlock.Parser p, Bundle into, TypedValue v) {
        String name = p.getAttributeValue(NS, "name");
        if (name == null) return;
        int vi = p.indexOf(NS, "value"), ri = p.indexOf(NS, "resource");
        if (ri >= 0) {
            into.putInt(name, p.getAttributeResourceValue(ri, 0));
        } else if (vi >= 0) {
            p.getAttributeTypedValue(vi, v);
            if (v.type == TypedValue.TYPE_STRING) into.putString(name, String.valueOf(v.string));
            else if (v.type == TypedValue.TYPE_INT_BOOLEAN) into.putBoolean(name, v.data != 0);
            else if (v.type == TypedValue.TYPE_FLOAT) into.putFloat(name, v.getFloat());
            else into.putInt(name, v.data);
        }
    }

    /* A permission's, a group's or an instrumentation's name, label and icon. */
    private static void item(XmlBlock.Parser p, PackageItemInfo info, String pkg) {
        info.name = full(p.getAttributeValue(NS, "name"), pkg);
        info.packageName = pkg;
        info.labelRes = p.getAttributeResourceValue(NS, "label", 0);
        if (info.labelRes == 0) info.nonLocalizedLabel = p.getAttributeValue(NS, "label");
        info.icon = p.getAttributeResourceValue(NS, "icon", 0);
    }

    private static String full(String n, String pkg) {
        if (n == null || n.isEmpty()) return null;
        if (n.startsWith(".")) return pkg + n;
        if (n.indexOf('.') < 0) return pkg + "." + n;
        return n;
    }

    /* android:taskAffinity as Android's buildTaskAffinityName reads it (megler.Manifest.affinity). */
    private static String affinity(String named, String byDefault, String pkg) {
        if (named == null) return byDefault;
        if (named.isEmpty()) return null;
        return named.startsWith(":") ? pkg + named : named;
    }

    public static class PackageParserException extends Exception {
        public final int error;
        public PackageParserException(int error, String detailMessage) { super(detailMessage); this.error = error; }
        public PackageParserException(int error, String detailMessage, Throwable throwable) { super(detailMessage, throwable); this.error = error; }
    }

    /** An APK's package as Android 9's PackageParser gives it. */
    public static final class Package {
        public String packageName, manifestPackageName, volumeUuid, codePath, baseCodePath, mVersionName, mSharedUserId, mRealPackage,
                mRestrictedAccountType, mRequiredAccountType, mOverlayTarget, mOverlayCategory, mCompileSdkVersionCodename, cpuAbiOverride,
                staticSharedLibName;
        public String[] splitNames, splitCodePaths, usesLibraryFiles;
        public int[] splitFlags, splitPrivateFlags, splitRevisionCodes;
        public int baseRevisionCode, mVersionCode, mVersionCodeMajor, mSharedUserLabel, mPreferredOrder, installLocation = -1, mOverlayPriority,
                mCompileSdkVersion;
        public long staticSharedLibVersion;
        public boolean baseHardwareAccelerated, coreApp, mRequiredForAllUsers, mOverlayIsStatic, use32bitAbi, isStub, visibleToInstantApps;
        public ApplicationInfo applicationInfo = new ApplicationInfo();
        public final ArrayList<Permission> permissions = new ArrayList<>();
        public final ArrayList<PermissionGroup> permissionGroups = new ArrayList<>();
        public final ArrayList<Activity> activities = new ArrayList<>();
        public final ArrayList<Activity> receivers = new ArrayList<>();
        public final ArrayList<Provider> providers = new ArrayList<>();
        public final ArrayList<Service> services = new ArrayList<>();
        public final ArrayList<Instrumentation> instrumentation = new ArrayList<>();
        public final ArrayList<String> requestedPermissions = new ArrayList<>();
        public ArrayList<String> protectedBroadcasts, libraryNames, mOriginalPackages, mAdoptPermissions;
        public ArrayList<String> usesLibraries = new ArrayList<>(), usesOptionalLibraries = new ArrayList<>();
        public ArrayList<ConfigurationInfo> configPreferences;
        public ArrayList<FeatureInfo> reqFeatures;
        public ArrayList<FeatureGroupInfo> featureGroups;
        public ArrayList<Package> childPackages;
        public Package parentPackage;
        public Bundle mAppMetaData;
        public Object mExtras;

        public Package(String packageName) {
            this.packageName = this.manifestPackageName = packageName;
            applicationInfo.packageName = packageName;
            applicationInfo.processName = packageName;
        }

        public long getLongVersionCode() { return ((long) mVersionCodeMajor << 32) | (mVersionCode & 0xffffffffL); }
        public List<String> getAllCodePaths() {
            ArrayList<String> paths = new ArrayList<>();
            if (baseCodePath != null) paths.add(baseCodePath);
            if (splitCodePaths != null) Collections.addAll(paths, splitCodePaths);
            return paths;
        }
        public List<String> getAllCodePathsExcludingResourceOnly() { return getAllCodePaths(); }
        public List<String> getChildPackageNames() { return null; }
        public boolean hasChildPackage(String packageName) { return false; }
        public void setPackageName(String newName) {
            packageName = newName;
            applicationInfo.packageName = newName;
            for (Component<?> c : components()) c.setPackageName(newName);
        }
        public void setCodePath(String codePath) { this.codePath = codePath; applicationInfo.setCodePath(codePath); }
        public void setBaseCodePath(String baseCodePath) { this.baseCodePath = baseCodePath; applicationInfo.setBaseCodePath(baseCodePath); }
        public boolean hasComponentClassName(String name) {
            for (Component<?> c : components()) if (name.equals(c.className)) return true;
            return false;
        }
        public boolean isSystem() { return (applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0; }
        public boolean isUpdatedSystemApp() { return (applicationInfo.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0; }
        public boolean isExternal() { return (applicationInfo.flags & ApplicationInfo.FLAG_EXTERNAL_STORAGE) != 0; }
        public boolean isForwardLocked() { return false; }
        public boolean isPrivileged() { return false; }
        public boolean isOem() { return false; }
        public boolean isVendor() { return false; }
        public boolean isProduct() { return false; }
        public boolean isLibrary() { return staticSharedLibName != null || (libraryNames != null && !libraryNames.isEmpty()); }
        public boolean canHaveOatDir() { return true; }
        private List<Component<?>> components() {
            ArrayList<Component<?>> all = new ArrayList<>();
            all.addAll(activities);
            all.addAll(receivers);
            all.addAll(providers);
            all.addAll(services);
            all.addAll(instrumentation);
            all.addAll(permissions);
            all.addAll(permissionGroups);
            return all;
        }
        @Override public String toString() { return "Package{" + Integer.toHexString(System.identityHashCode(this)) + " " + packageName + "}"; }
    }

    /** A component of a package: its class, its filters, its meta-data. */
    public static abstract class Component<II extends IntentInfo> {
        public final ArrayList<II> intents;
        public final String className;
        public Bundle metaData;
        public Package owner;
        public int order;
        ComponentName componentName;
        String componentShortName;
        /* (What the parser keeps: the manifest's tag, and whether android:exported was given.) */
        String tag = "";
        boolean exportedSet;

        public Component(Package owner) { this(owner, null); }
        Component(Package owner, String className) {
            this.owner = owner;
            this.className = className;
            this.intents = new ArrayList<>(0);
        }
        public Component(Component<II> clone) {
            owner = clone.owner;
            intents = clone.intents;
            className = clone.className;
            componentName = clone.componentName;
            componentShortName = clone.componentShortName;
            metaData = clone.metaData;
        }
        public ComponentName getComponentName() {
            if (componentName == null && className != null) componentName = new ComponentName(owner.applicationInfo.packageName, className);
            return componentName;
        }
        public void setPackageName(String packageName) { componentName = null; componentShortName = null; }
        public void appendComponentShortName(StringBuilder sb) { ComponentName n = getComponentName(); if (n != null) sb.append(n.flattenToShortString()); }
        public void printComponentShortName(java.io.PrintWriter pw) { ComponentName n = getComponentName(); if (n != null) pw.print(n.flattenToShortString()); }
        /* The PackageItemInfo it describes. */
        abstract PackageItemInfo info();
        /* A filter of its kind, added to it. */
        II newIntent() { throw new UnsupportedOperationException(); }
    }

    public static final class Activity extends Component<ActivityIntentInfo> {
        public final ActivityInfo info;
        Activity(Package owner, String className, ActivityInfo info, String tag) { super(owner, className); this.info = info; this.tag = tag; }
        @Override PackageItemInfo info() { return info; }
        @Override ActivityIntentInfo newIntent() { ActivityIntentInfo i = new ActivityIntentInfo(this); intents.add(i); return i; }
        @Override public void setPackageName(String packageName) { super.setPackageName(packageName); info.packageName = packageName; }
        @Override public String toString() { return "Activity{" + Integer.toHexString(System.identityHashCode(this)) + " " + className + "}"; }
    }

    public static final class Service extends Component<ServiceIntentInfo> {
        public final ServiceInfo info;
        Service(Package owner, String className, ServiceInfo info) { super(owner, className); this.info = info; }
        @Override PackageItemInfo info() { return info; }
        @Override ServiceIntentInfo newIntent() { ServiceIntentInfo i = new ServiceIntentInfo(this); intents.add(i); return i; }
        @Override public void setPackageName(String packageName) { super.setPackageName(packageName); info.packageName = packageName; }
        @Override public String toString() { return "Service{" + Integer.toHexString(System.identityHashCode(this)) + " " + className + "}"; }
    }

    public static final class Provider extends Component<ProviderIntentInfo> {
        public final ProviderInfo info;
        public boolean syncable;
        Provider(Package owner, String className, ProviderInfo info) { super(owner, className); this.info = info; }
        public Provider(Provider existingProvider) { super(existingProvider); info = existingProvider.info; syncable = existingProvider.syncable; }
        @Override PackageItemInfo info() { return info; }
        @Override ProviderIntentInfo newIntent() { ProviderIntentInfo i = new ProviderIntentInfo(this); intents.add(i); return i; }
        @Override public void setPackageName(String packageName) { super.setPackageName(packageName); info.packageName = packageName; }
        @Override public String toString() { return "Provider{" + Integer.toHexString(System.identityHashCode(this)) + " " + info.name + "}"; }
    }

    public static final class Permission extends Component<IntentInfo> {
        public final PermissionInfo info = new PermissionInfo();
        public boolean tree;
        public PermissionGroup group;
        public Permission(Package owner) { super(owner, null); }
        @Override PackageItemInfo info() { return info; }
        @Override public String toString() { return "Permission{" + Integer.toHexString(System.identityHashCode(this)) + " " + info.name + "}"; }
    }

    public static final class PermissionGroup extends Component<IntentInfo> {
        public final PermissionGroupInfo info = new PermissionGroupInfo();
        public PermissionGroup(Package owner) { super(owner, null); }
        @Override PackageItemInfo info() { return info; }
        @Override public String toString() { return "PermissionGroup{" + Integer.toHexString(System.identityHashCode(this)) + " " + info.name + "}"; }
    }

    public static final class Instrumentation extends Component<IntentInfo> {
        public final InstrumentationInfo info = new InstrumentationInfo();
        public Instrumentation(Package owner) { super(owner, null); }
        @Override PackageItemInfo info() { return info; }
        @Override public String toString() { return "Instrumentation{" + Integer.toHexString(System.identityHashCode(this)) + " " + info.name + "}"; }
    }

    /** An <intent-filter> of a component, with what Android's parser keeps of it besides the filter. */
    public static abstract class IntentInfo extends IntentFilter {
        public boolean hasDefault;
        public int labelRes, icon, logo, banner, preferred;
        public CharSequence nonLocalizedLabel;
        protected IntentInfo() {}
    }

    public static final class ActivityIntentInfo extends IntentInfo {
        public Activity activity;
        public ActivityIntentInfo(Activity activity) { this.activity = activity; }
        @Override public String toString() { return "ActivityIntentInfo{" + Integer.toHexString(System.identityHashCode(this)) + " " + activity + "}"; }
    }

    public static final class ServiceIntentInfo extends IntentInfo {
        public Service service;
        public ServiceIntentInfo(Service service) { this.service = service; }
        @Override public String toString() { return "ServiceIntentInfo{" + Integer.toHexString(System.identityHashCode(this)) + " " + service + "}"; }
    }

    public static final class ProviderIntentInfo extends IntentInfo {
        public Provider provider;
        public ProviderIntentInfo(Provider provider) { this.provider = provider; }
        @Override public String toString() { return "ProviderIntentInfo{" + Integer.toHexString(System.identityHashCode(this)) + " " + provider + "}"; }
    }
}
