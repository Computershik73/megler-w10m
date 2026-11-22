package megler;

import android.content.res.XmlBlock;
import android.os.Bundle;
import android.util.TypedValue;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;

/** The app's AndroidManifest.xml, read once: the application and its components, as PackageManager would report them. */
public final class Manifest {
    public static final class Component {
        public String name, kind, authorities, process, permission, readPermission, writePermission, parentActivity;
        /** An activity's android:taskAffinity (affinity() says how it is read); null when it has none. */
        public String taskAffinity;
        public int theme, label, icon, screenOrientation = -1, configChanges, launchMode, windowSoftInputMode, uiOptions, initOrder;
        public boolean exported, enabled = true, grantUriPermissions;
        /** android:enabled when it is a boolean resource of the app's (read by PackageManager once there are resources); 0 for a literal. */
        public int enabledResource;
        public Bundle metaData;
        public final ArrayList<Filter> filters = new ArrayList<>();
    }
    /** An <intent-filter> as Android's IntentFilter has it (megler.IntentMatch matches one). */
    public static final class Filter {
        public final ArrayList<String> actions = new ArrayList<>(), categories = new ArrayList<>(), schemes = new ArrayList<>(), mimeTypes = new ArrayList<>(), hosts = new ArrayList<>();
        /** Each host's port, -1 for none: Android's AuthorityEntry is a <data>'s host with that <data>'s port. */
        public final ArrayList<Integer> ports = new ArrayList<>();
        /** Its paths and scheme-specific parts, as android.os.PatternMatcher matches them. */
        public final ArrayList<IntentMatch.Part> paths = new ArrayList<>(), ssps = new ArrayList<>();
        public int priority;
        /** android:autoVerify: its web links are for this app once their domains say so (megler.AssetLinks). */
        public boolean autoVerify;
    }
    public static String packageName, applicationClass, versionName, appComponentFactory, appTaskAffinity;
    public static int versionCode, minSdk = 21, targetSdk = 34, appTheme, appLabel, appIcon, appFlags;
    public static boolean debuggable, largeHeap, hardwareAccelerated = true, extractNativeLibs = true;
    /** Whether the manifest has <uses-sdk>: one without is an app of Android 1's, whose target is that (megler.Permissions). */
    public static boolean usesSdk;
    public static Bundle appMetaData = new Bundle();
    public static final ArrayList<Component> activities = new ArrayList<>(), services = new ArrayList<>(), receivers = new ArrayList<>(), providers = new ArrayList<>();
    public static final ArrayList<String> permissions = new ArrayList<>(), libraries = new ArrayList<>();
    private static boolean sRead;

    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static String full(String n) {
        if (n == null) return null;
        if (n.startsWith(".")) return packageName + n;
        if (n.indexOf('.') < 0) return packageName + "." + n;
        return n;
    }
    /** The first of the app's split APKs among Native's APKs (mg_java.c FIRST_SPLIT_APK): the base is 0, framework-res 1. */
    private static final int FIRST_SPLIT_APK = 2;

    public static synchronized void read() {
        if (sRead) return;
        sRead = true;
        byte[] d = Native.readApkFile(0, "AndroidManifest.xml");
        if (d == null) return;
        parse(d, false);
        /*
         * And the components each split declares, as Android's package parser adds a feature split's to the app's (its
         * manifest and application say nothing more of the app): Chrome 158 (isolatedSplits) has its browser,
         * ChromeTabbedActivity and the launcher alias com.google.android.apps.chrome.Main, in its split "chrome". The
         * split's code is in the app's class loader with the base's (mg_java.c, mg_dvm_add_apk).
         */
        String[] splits = Native.splitPaths();
        for (int k = 0; splits != null && k < splits.length; k++) {
            byte[] m = Native.readApkFile(FIRST_SPLIT_APK + k, "AndroidManifest.xml");
            if (m != null) parse(m, true);
        }
    }

    private static void parse(byte[] d, boolean split) {
        try {
            XmlBlock.Parser p = new XmlBlock(d, "AndroidManifest.xml").newParser();
            Component cur = null; Filter filter = null;
            boolean inApp = false;          /* components are only those under <application> (<queries> names packages, intents and providers too) */
            int t;
            TypedValue v = new TypedValue();
            while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                if (t == XmlPullParser.END_TAG) {
                    String n = p.getName();
                    if ("intent-filter".equals(n)) filter = null;
                    else if ("application".equals(n)) inApp = false;
                    else if ("activity".equals(n) || "activity-alias".equals(n) || "service".equals(n) || "receiver".equals(n) || "provider".equals(n)) cur = null;
                    continue;
                }
                if (t != XmlPullParser.START_TAG) continue;
                String tag = p.getName();
                if (split) {
                    // A split's manifest: its components alone.
                    if ("application".equals(tag)) { inApp = true; continue; }
                    if ("manifest".equals(tag) || tag.startsWith("uses-")) continue;
                    if ("meta-data".equals(tag) && cur == null) continue;
                }
                switch (tag) {
                case "manifest":
                    packageName = p.getAttributeValue(null, "package");
                    versionCode = p.getAttributeIntValue(NS, "versionCode", 1);
                    versionName = p.getAttributeValue(NS, "versionName");
                    break;
                case "uses-sdk":
                    usesSdk = true;
                    minSdk = p.getAttributeIntValue(NS, "minSdkVersion", 21);
                    targetSdk = p.getAttributeIntValue(NS, "targetSdkVersion", minSdk);
                    break;
                /*
                 * (uses-permission-sdk-23 asks only on Android 6 and later, which this is: WhatsApp asks for READ_CALL_LOG so.) Each
                 * once, and as Android's package parser takes them: one whose android:maxSdkVersion is below the Android this is
                 * (Build.VERSION.SDK_INT) is not asked for here -- WRITE_EXTERNAL_STORAGE up to 28 and READ_EXTERNAL_STORAGE up to
                 * 32, of apps that ask for the media permissions on later ones (megler.Permissions denies it).
                 */
                case "uses-permission": case "uses-permission-sdk-23": case "uses-permission-sdk-m": {
                    String name = p.getAttributeValue(NS, "name");
                    int max = p.getAttributeIntValue(NS, "maxSdkVersion", 0);
                    if (name != null && (max <= 0 || max >= android.os.Build.VERSION.SDK_INT) && !permissions.contains(name)) permissions.add(name);
                    break;
                }
                case "uses-library": libraries.add(p.getAttributeValue(NS, "name")); break;
                case "application":
                    inApp = true;
                    applicationClass = full(p.getAttributeValue(NS, "name"));
                    appTheme = p.getAttributeResourceValue(NS, "theme", 0);
                    appLabel = p.getAttributeResourceValue(NS, "label", 0);
                    appIcon = p.getAttributeResourceValue(NS, "icon", 0);
                    debuggable = p.getAttributeBooleanValue(NS, "debuggable", false);
                    largeHeap = p.getAttributeBooleanValue(NS, "largeHeap", false);
                    extractNativeLibs = p.getAttributeBooleanValue(NS, "extractNativeLibs", true);
                    hardwareAccelerated = p.getAttributeBooleanValue(NS, "hardwareAccelerated", true);
                    appComponentFactory = full(p.getAttributeValue(NS, "appComponentFactory"));
                    appTaskAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), packageName, packageName);
                    break;
                case "activity": case "activity-alias": case "service": case "receiver": case "provider": {
                    if (!inApp) break;
                    cur = new Component();
                    cur.kind = tag;
                    cur.name = full(p.getAttributeValue(NS, "name"));
                    cur.taskAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), appTaskAffinity, packageName);
                    if ("activity-alias".equals(tag)) {
                        String target = full(p.getAttributeValue(NS, "targetActivity"));
                        for (Component a : activities) if (a.name.equals(target)) { cur.theme = a.theme; cur.screenOrientation = a.screenOrientation; cur.configChanges = a.configChanges; cur.taskAffinity = a.taskAffinity; }
                        cur.parentActivity = target;
                    }
                    cur.theme = p.getAttributeResourceValue(NS, "theme", cur.theme);
                    cur.label = p.getAttributeResourceValue(NS, "label", 0);
                    cur.icon = p.getAttributeResourceValue(NS, "icon", 0);
                    cur.screenOrientation = p.getAttributeIntValue(NS, "screenOrientation", cur.screenOrientation);
                    cur.configChanges = p.getAttributeIntValue(NS, "configChanges", cur.configChanges);
                    cur.launchMode = p.getAttributeIntValue(NS, "launchMode", 0);
                    cur.initOrder = p.getAttributeIntValue(NS, "initOrder", 0);
                    cur.windowSoftInputMode = p.getAttributeIntValue(NS, "windowSoftInputMode", 0);
                    cur.exported = p.getAttributeBooleanValue(NS, "exported", false);
                    cur.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                    cur.enabledResource = p.getAttributeResourceValue(NS, "enabled", 0);
                    cur.authorities = p.getAttributeValue(NS, "authorities");
                    cur.grantUriPermissions = p.getAttributeBooleanValue(NS, "grantUriPermissions", false);
                    cur.process = p.getAttributeValue(NS, "process");
                    cur.permission = p.getAttributeValue(NS, "permission");
                    cur.readPermission = p.getAttributeValue(NS, "readPermission");
                    cur.writePermission = p.getAttributeValue(NS, "writePermission");
                    if (cur.parentActivity == null) cur.parentActivity = full(p.getAttributeValue(NS, "parentActivityName"));
                    if ("service".equals(tag)) services.add(cur);
                    else if ("receiver".equals(tag)) receivers.add(cur);
                    else if ("provider".equals(tag)) providers.add(cur);
                    else activities.add(cur);
                    break;
                }
                case "intent-filter": if (cur != null) { filter = new Filter(); cur.filters.add(filter); filterAttributes(p, filter); } break;
                case "action": if (filter != null) filter.actions.add(p.getAttributeValue(NS, "name")); break;
                case "category": if (filter != null) filter.categories.add(p.getAttributeValue(NS, "name")); break;
                case "data": if (filter != null) data(p, filter); break;
                case "meta-data": {
                    String name = p.getAttributeValue(NS, "name");
                    Bundle target = cur != null ? (cur.metaData != null ? cur.metaData : (cur.metaData = new Bundle())) : appMetaData;
                    int vi = p.indexOf(NS, "value"), ri = p.indexOf(NS, "resource");
                    if (name == null) break;
                    if (ri >= 0) target.putInt(name, p.getAttributeResourceValue(ri, 0));
                    else if (vi >= 0) {
                        p.getAttributeTypedValue(vi, v);
                        // a reference (android:value="@integer/google_play_services_version") is resolved, as Android's parser does
                        if (v.type == TypedValue.TYPE_REFERENCE && v.data != 0) {
                            try { android.content.res.Resources r = megler.AppRunner.application() != null ? megler.AppRunner.application().getResources() : android.content.res.Resources.getSystem(); r.getValue(v.data, v, true); }
                            catch (RuntimeException e) { /* left as the id */ }
                        }
                        if (v.type == TypedValue.TYPE_STRING) target.putString(name, String.valueOf(v.string));
                        else if (v.type == TypedValue.TYPE_INT_BOOLEAN) target.putBoolean(name, v.data != 0);
                        else if (v.type == TypedValue.TYPE_FLOAT) target.putFloat(name, v.getFloat());
                        else if (v.type == TypedValue.TYPE_REFERENCE) target.putInt(name, v.data);
                        else target.putInt(name, v.data);
                    }
                    break;
                }
                }
            }
        } catch (Exception e) { android.util.Log.e("Megler", "the manifest could not be read", e); }
    }
    public static Component activity(String cls) { read(); for (Component c : activities) if (c.name.equals(cls)) return c; return null; }
    public static Component launcher() {
        read();
        for (Component c : activities) for (Filter f : c.filters) if (f.categories.contains("android.intent.category.LAUNCHER") && f.actions.contains("android.intent.action.MAIN")) return c;
        for (Component c : activities) for (Filter f : c.filters) if (f.categories.contains("android.intent.category.LAUNCHER")) return c;
        return activities.isEmpty() ? null : activities.get(0);
    }
    /** The services (or receivers) of this app an intent names or whose filters take it. */
    public static java.util.List<Component> match(java.util.List<Component> list, android.content.Intent i) {
        read();
        java.util.ArrayList<Component> out = new java.util.ArrayList<>();
        android.content.ComponentName cn = i.getComponent();
        if (cn != null) {
            if (cn.getPackageName() != null && !cn.getPackageName().equals(megler.Native.packageName())) return out;
            for (Component c : list) if (c.name.equals(cn.getClassName())) { out.add(c); return out; }
            return out;
        }
        String action = i.getAction();
        if (action == null) return out;
        if (i.getPackage() != null && !i.getPackage().equals(megler.Native.packageName())) return out;
        for (Component c : list) for (Filter f : c.filters) if (matches(f, i)) { out.add(c); break; }
        return out;
    }
    /* an attribute that may name a string resource (Shazam's <data android:scheme="@string/...">): kept as "@<id>", read when used */
    private static String attrOrRef(XmlBlock.Parser p, String name) {
        int ref = p.getAttributeResourceValue(NS, name, 0);
        if (ref != 0) return "@" + ref;
        return p.getAttributeValue(NS, name);
    }
    /* An <intent-filter>'s own attributes: its priority and whether its web links are to be verified. */
    private static void filterAttributes(XmlBlock.Parser p, Filter f) {
        f.priority = p.getAttributeIntValue(NS, "priority", 0);
        f.autoVerify = p.getAttributeBooleanValue(NS, "autoVerify", false);
    }
    /*
     * A <data> of a filter, as Android's package parser adds it: its scheme, its MIME type, its host with its port (a port
     * without a host is not looked at), its path and its scheme-specific part, each of the kinds PatternMatcher has.
     */
    private static void data(XmlBlock.Parser p, Filter f) {
        String s = attrOrRef(p, "scheme"), h = attrOrRef(p, "host"), m = attrOrRef(p, "mimeType");
        if (s != null) f.schemes.add(s);
        if (m != null) f.mimeTypes.add(m);
        if (h != null) {
            f.hosts.add(h);
            int port = -1;
            String text = val(attrOrRef(p, "port"));
            if (text != null) try { port = Integer.parseInt(text.trim()); } catch (NumberFormatException e) {}
            f.ports.add(port);
        }
        for (int k = 0; k < IntentMatch.PATH_ATTRS.length; k++) {
            String v = attrOrRef(p, IntentMatch.PATH_ATTRS[k]);
            if (v != null) f.paths.add(new IntentMatch.Part(k, v));
        }
        for (int k = 0; k < IntentMatch.SSP_ATTRS.length; k++) {
            String v = attrOrRef(p, IntentMatch.SSP_ATTRS[k]);
            if (v != null) f.ssps.add(new IntentMatch.Part(k, v));
        }
    }
    /** A value of the manifest's, read where it names a string resource ("@<id>"). */
    public static String val(String v) {
        if (v == null || !v.startsWith("@")) return v;
        try { return megler.ContextImpl.app().getResources().getString(Integer.parseInt(v.substring(1))); } catch (Throwable t) { return v; }
    }
    /** The process a component runs in, as Android names it: the app's own when none is named (or ""), "<package>:x" for
     *  ":x", and a string resource's text when the manifest names one (GmsCore's "@string/common_persistent_process"). An
     *  <activity-alias> runs in its target's, as Android's PackageParser gives it the target's process: the Google app's
     *  RobinShellAppEntryPointActivity, which names none, is its RobinEntryPointActivity's, in ":search". */
    public static String processOf(Component c) {
        if ("activity-alias".equals(c.kind) && c.parentActivity != null) {
            Component target = activity(c.parentActivity);
            if (target != null && !"activity-alias".equals(target.kind)) c = target;
        }
        String pkg = megler.Native.packageName(), p = val(c.process);
        return p == null || p.isEmpty() ? pkg : p.startsWith(":") ? pkg + p : p;
    }
    /** Android's intent-filter match (megler.IntentMatch): the action, the data -- scheme, scheme-specific part, host and port,
     *  path, MIME type -- and the categories, each one the intent names being the filter's (IntentFilter.matchCategories).
     *  The categories were not looked at: an intent for the phone's home screen (MAIN with CATEGORY_HOME) found the app's own
     *  launcher activity (MAIN with CATEGORY_LAUNCHER), which Facebook's Back on its sign-in and first-run screens, going home
     *  through a launcher of another signature, refused as its own -- and then ended the app rather than going home. */
    static boolean matches(Filter f, android.content.Intent i) {
        return IntentMatch.matches(f, i, IntentMatch.typeOf(i), false);
    }
    /** The activity an explicit or implicit intent would open in this app, or null. */
    public static Component resolve(android.content.Intent i) {
        read();
        // (An implicit intent for another package is not for this app, though a filter of its own matches it: a Trusted Web
        // Activity's launcher asks the browser it found, com.android.browser, for its site by package, and MangaDex, whose
        // launcher has a filter for that site, was started again and again by its own request, its screen left blank.)
        if (i.getComponent() == null && i.getPackage() != null && !i.getPackage().equals(megler.Native.packageName())) return null;
        return resolveIn(activities, i);
    }
    /** The activity of these an explicit or implicit intent would open, or null. */
    public static Component resolveIn(java.util.List<Component> list, android.content.Intent i) {
        android.content.ComponentName cn = i.getComponent();
        if (cn != null) {
            for (Component c : list) if (c.name.equals(cn.getClassName())) return c;
            for (Component a : list) if (a.name.endsWith(cn.getClassName())) return a;
            return null;
        }
        for (Component c : list) for (Filter f : c.filters) if (matches(f, i)) return c;
        return null;
    }

    /**
     * The activities (and their aliases) of another APK's manifest, package `pkg`: what megler.MicroG starts of Google Play
     * services in an app that hosts it. An activity that names no theme has its application's, as on Android.
     */
    public static ArrayList<Component> activitiesIn(byte[] manifest, String label, String pkg) {
        ArrayList<Component> out = new ArrayList<>();
        try {
            XmlBlock.Parser p = new XmlBlock(manifest, label).newParser();
            Component cur = null; Filter filter = null;
            boolean inApp = false;
            int t, appTheme = 0;
            String appAffinity = pkg;
            while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                String tag = p.getName();
                if (t == XmlPullParser.END_TAG) {
                    if ("intent-filter".equals(tag)) filter = null;
                    else if ("application".equals(tag)) inApp = false;
                    else if ("activity".equals(tag) || "activity-alias".equals(tag)) cur = null;
                    continue;
                }
                if (t != XmlPullParser.START_TAG) continue;
                switch (tag) {
                case "application":
                    inApp = true;
                    appTheme = p.getAttributeResourceValue(NS, "theme", 0);
                    appAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), pkg, pkg);
                    break;
                case "activity": case "activity-alias": {
                    if (!inApp) break;
                    cur = new Component();
                    cur.kind = tag;
                    cur.name = full(p.getAttributeValue(NS, "name"), pkg);
                    cur.theme = appTheme;
                    cur.taskAffinity = affinity(p.getAttributeValue(NS, "taskAffinity"), appAffinity, pkg);
                    if ("activity-alias".equals(tag)) {
                        String target = full(p.getAttributeValue(NS, "targetActivity"), pkg);
                        for (Component a : out) if (a.name.equals(target)) { cur.theme = a.theme; cur.screenOrientation = a.screenOrientation; cur.configChanges = a.configChanges; cur.taskAffinity = a.taskAffinity; }
                        cur.parentActivity = target;
                    }
                    cur.theme = p.getAttributeResourceValue(NS, "theme", cur.theme);
                    cur.label = p.getAttributeResourceValue(NS, "label", 0);
                    cur.icon = p.getAttributeResourceValue(NS, "icon", 0);
                    cur.screenOrientation = p.getAttributeIntValue(NS, "screenOrientation", cur.screenOrientation);
                    cur.configChanges = p.getAttributeIntValue(NS, "configChanges", cur.configChanges);
                    cur.launchMode = p.getAttributeIntValue(NS, "launchMode", 0);
                    cur.windowSoftInputMode = p.getAttributeIntValue(NS, "windowSoftInputMode", 0);
                    cur.exported = p.getAttributeBooleanValue(NS, "exported", false);
                    cur.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                    cur.process = p.getAttributeValue(NS, "process");
                    cur.permission = p.getAttributeValue(NS, "permission");
                    out.add(cur);
                    break;
                }
                case "intent-filter": if (cur != null) { filter = new Filter(); cur.filters.add(filter); filterAttributes(p, filter); } break;
                case "action": if (filter != null) filter.actions.add(p.getAttributeValue(NS, "name")); break;
                case "category": if (filter != null) filter.categories.add(p.getAttributeValue(NS, "name")); break;
                case "data": if (filter != null) otherData(p, filter); break;
                }
            }
        } catch (Exception e) { android.util.Log.e("Megler", "the manifest of " + pkg + " could not be read", e); }
        return out;
    }
    /* A <data> of another APK's filter: its values as they are, its string resources not being this app's. */
    private static void otherData(XmlBlock.Parser p, Filter f) {
        String s = p.getAttributeValue(NS, "scheme"), h = p.getAttributeValue(NS, "host"), m = p.getAttributeValue(NS, "mimeType");
        if (s != null) f.schemes.add(s);
        if (m != null) f.mimeTypes.add(m);
        if (h != null) {
            f.hosts.add(h);
            int port = -1;
            String text = p.getAttributeValue(NS, "port");
            if (text != null) try { port = Integer.parseInt(text.trim()); } catch (NumberFormatException e) {}
            f.ports.add(port);
        }
        for (int k = 0; k < IntentMatch.PATH_ATTRS.length; k++) {
            String v = p.getAttributeValue(NS, IntentMatch.PATH_ATTRS[k]);
            if (v != null) f.paths.add(new IntentMatch.Part(k, v));
        }
        for (int k = 0; k < IntentMatch.SSP_ATTRS.length; k++) {
            String v = p.getAttributeValue(NS, IntentMatch.SSP_ATTRS[k]);
            if (v != null) f.ssps.add(new IntentMatch.Part(k, v));
        }
    }
    private static String full(String n, String pkg) {
        if (n == null) return null;
        if (n.startsWith(".")) return pkg + n;
        if (n.indexOf('.') < 0) return pkg + "." + n;
        return n;
    }
    /*
     * android:taskAffinity as Android's PackageParser.buildTaskAffinityName reads it: none named is the default (an activity
     * has its application's, an application its package's), an empty one is no affinity at all, and ":x" is the package's own
     * "<package>:x". Activity.finishAffinity finishes the activities of the caller's affinity (megler.AppRunner).
     */
    private static String affinity(String named, String byDefault, String pkg) {
        if (named == null) return byDefault;
        if (named.isEmpty()) return null;
        return named.startsWith(":") ? pkg + named : named;
    }
}
