// Megler: what an implicit intent with data opens -- a link, a mailto:, geo: or market: address, a file of a type --, as
// Android's activity resolver answers startActivity (megler.AppRunner) and PackageManager.queryIntentActivities:
//
//  - Who can: the app's own activities and the other apps' (megler.PhoneApps, as the shell described their filters) whose
//    filters take the intent, CATEGORY_DEFAULT among them (MATCH_DEFAULT_ONLY), matched as Android matches them
//    (megler.IntentMatch: path, port, type and all). An intent that names a package or a class is not this one's.
//  - A web link (http or https, megler.IntentMatch.isDomainVerificationIntent) goes, as on Android 12 and later, to the apps
//    approved for its domain -- verified for it (android:autoVerify, megler.AssetLinks) or allowed by the user ("links.<package>
//    =all", megler.Defaults) -- and to the default browser when none is: the one the user chose, else Windows' own. An app's
//    filter for a site is no approval: Komi Store's for github.com/<owner>/<repo>, not verified, took its own sign-in page,
//    https://github.com/login/device, which never opened (issue #12), and an app opens no link it is not approved for.
//  - Several left: the one the user said "Always" to for that kind of intent, else "Open with" (megler.Chooser), where
//    "Always" is remembered (Defaults). One: it. None: what AppRunner did before -- Windows opens the address.
//
// Another app's activity is started by the shell, with the intent (megler.PhoneApps.start); Windows' browser is Native.openUrl.
// The log says where each went and why, with the link's scheme and host only: a link may carry a code or a token.
package megler;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import java.util.ArrayList;
import java.util.List;

public final class Resolver {
    private Resolver() {}

    /** One that can take the intent: an activity of this app's, one of another app's, or Windows. */
    public static final class Target {
        /** The package and the activity, an alias's own name; both null for Windows. */
        public final String pkg, cls;
        final Manifest.Component own;
        final PhoneApps.App app;
        final PhoneApps.Activity act;
        final Manifest.Filter filter;

        Target(Manifest.Component own, Manifest.Filter f) {
            this.pkg = Native.packageName(); this.cls = own.name; this.own = own; this.app = null; this.act = null; this.filter = f;
        }
        Target(PhoneApps.App app, PhoneApps.Activity act, Manifest.Filter f) {
            this.pkg = app.packageName; this.cls = act.name; this.own = null; this.app = app; this.act = act; this.filter = f;
        }
        private Target() { pkg = null; cls = null; own = null; app = null; act = null; filter = null; }

        public boolean windows() { return pkg == null; }
        boolean browser() { return windows() || IntentMatch.handlesAllWebData(filter); }
        /** "<package>/<activity>", or "windows": what Defaults keeps. */
        public String id() { return windows() ? Defaults.WINDOWS : pkg + "/" + cls; }

        public CharSequence label() {
            if (windows()) return "Windows";
            if (app != null) return act.label != null ? act.label : app.label;
            try {
                Context c = ContextImpl.app();
                if (own.label != 0) return c.getResources().getText(own.label);
                if (Manifest.appLabel != 0) return c.getResources().getText(Manifest.appLabel);
            } catch (RuntimeException e) {}
            return pkg;
        }

        public android.graphics.drawable.Drawable icon() {
            try {
                if (app != null) return PhoneApps.icon(pkg, act.icon);
                if (own != null) {
                    Context c = ContextImpl.app();
                    int id = own.icon != 0 ? own.icon : Manifest.appIcon;
                    if (id != 0) return c.getResources().getDrawable(id);
                }
            } catch (RuntimeException e) {}
            return null;
        }

        /** As PackageManager tells of it. */
        ResolveInfo resolveInfo() {
            ResolveInfo r = new ResolveInfo();
            if (windows()) {
                r.activityInfo = new ActivityInfo();
                r.activityInfo.name = "com.android.browser.Browser";
                r.activityInfo.packageName = "com.android.browser";
                r.activityInfo.exported = r.activityInfo.enabled = true;
                r.nonLocalizedLabel = "Browser";
            } else if (app != null) {
                r.activityInfo = PhoneApps.activityInfo(app, act);
            } else {
                r.activityInfo = android.content.pm.PackageManager.meglerActivityInfo(own);
            }
            if (filter != null) {
                r.priority = filter.priority;
                r.isDefault = filter.categories.contains(Intent.CATEGORY_DEFAULT);
                r.handleAllWebDataURI = IntentMatch.handlesAllWebData(filter);
            }
            r.match = 0x00108000; // MATCH_CATEGORY_SCHEME + MATCH_ADJUSTMENT_NORMAL
            return r;
        }

        @Override public String toString() { return windows() ? "Windows" : pkg + "/" + cls; }
    }

    static final Target WINDOWS = new Target();

    /** Whether the intent is one this resolves: implicit, for no package, with data or a type, and none of the system's own. */
    public static boolean applies(Intent i) {
        if (i.getComponent() != null || i.getPackage() != null) return false;
        if (i.getData() == null && i.getType() == null) return false;
        String a = i.getAction();
        if (a == null) return false;
        // (Sharing is Windows' share (Native.share), the chooser's intent is what it holds, and Android's Settings screens are
        // Megler's own (megler.SystemScreens).)
        if (Intent.ACTION_SEND.equals(a) || Intent.ACTION_SEND_MULTIPLE.equals(a) || Intent.ACTION_CHOOSER.equals(a) || a.startsWith("android.settings.")) return false;
        return !PhoneApps.isLauncherIntent(i);
    }

    /** Every activity, this app's and the others', whose filter takes the intent; with `defaultOnly`, one with CATEGORY_DEFAULT. */
    static List<Target> candidates(Intent i, String type, boolean defaultOnly) {
        ArrayList<Target> out = new ArrayList<>();
        Manifest.read();
        for (Manifest.Component c : Manifest.activities) {
            if (!c.enabled) continue;
            for (Manifest.Filter f : c.filters)
                if (IntentMatch.matches(f, i, type, defaultOnly)) { out.add(new Target(c, f)); break; }
        }
        for (PhoneApps.App a : PhoneApps.all()) {
            // (Google Play services' activities run in the app that asks, megler.MicroG.activity, before this is asked.)
            if (GooglePackages.GMS.equals(a.packageName) || GooglePackages.GSF.equals(a.packageName)) continue;
            List<PhoneApps.Activity> acts = PhoneApps.activities(a);
            if (acts == null) continue;
            for (PhoneApps.Activity x : acts) {
                if (!x.enabled || !x.exported) continue;
                Manifest.Filter f = x.filterFor(i, type, defaultOnly);
                if (f != null) out.add(new Target(a, x, f));
            }
        }
        return out;
    }

    /** What the resolution came to: those left, and why, in words; `unknown` when a domain's verification was not asked yet. */
    static final class Decision {
        final List<Target> targets = new ArrayList<>();
        String why = "";
        boolean unknown;
    }

    /**
     * Who the intent goes to (MATCH_DEFAULT_ONLY, startActivity's); `network`: a domain not verified yet is asked
     * (megler.AssetLinks), not on the main thread; without it, such an app is not approved and `unknown` is set.
     */
    static Decision decide(Intent i, boolean network) {
        Decision d = new Decision();
        List<Target> all = candidates(i, IntentMatch.typeOf(i), true);
        if (IntentMatch.isWebIntent(i)) {
            String host = i.getData().getHost();
            ArrayList<Target> browsers = new ArrayList<>(), others = new ArrayList<>();
            for (Target t : all) (t.browser() ? browsers : others).add(t);
            if (IntentMatch.isDomainVerificationIntent(i, true)) {
                int best = 0;
                ArrayList<Target> approved = new ArrayList<>();
                StringBuilder refused = new StringBuilder();
                for (Target t : others) {
                    int level = approval(t, host, network, d);
                    if (level == 0) refused.append(refused.length() > 0 ? ", " : "").append(t.label());
                    if (level > best) { approved.clear(); best = level; }
                    if (level > 0 && level == best) approved.add(t);
                }
                if (!approved.isEmpty()) {
                    d.targets.addAll(approved);
                    d.why = best == VERIFIED ? "verified for " + host : "allowed by the user to open links to " + host;
                } else {
                    d.targets.add(defaultBrowser(browsers));
                    d.why = refused.length() > 0 ? "the browser: " + refused + " is not approved for " + host + " (not verified, not allowed in its settings)"
                                                 : "the browser";
                }
            } else {
                d.targets.addAll(others);
                d.targets.add(defaultBrowser(browsers));
                d.why = "a web link of other categories: every app that takes it, and the browser";
            }
        } else {
            d.targets.addAll(all);
            d.why = all.size() == 1 ? "the one activity that takes it" : all.size() + " activities take it";
        }
        if (d.targets.size() > 1) {
            String always = Defaults.get(Defaults.ALWAYS + kind(i));
            if (always != null)
                for (Target t : d.targets)
                    if (t.id().equals(always)) {
                        d.targets.clear();
                        d.targets.add(t);
                        d.why = "the user's \"Always\" for " + kind(i);
                        break;
                    }
        }
        return d;
    }

    static final int VERIFIED = 2, SELECTED = 1;

    /* How far an app is approved for links to the domain (DomainVerificationService's approval levels): VERIFIED, SELECTED by
       the user, or 0. */
    private static int approval(Target t, String host, boolean network, Decision d) {
        String mode = Defaults.get(Defaults.LINKS + t.pkg);
        if ("off".equals(mode)) return 0;
        String domain = verifiedDomain(t, host);
        if (domain != null) {
            int state = AssetLinks.cached(t.pkg, domain);
            if (state == AssetLinks.UNKNOWN && network) state = AssetLinks.verify(t.pkg, domain, certificates(t));
            if (state == AssetLinks.UNKNOWN) d.unknown = true;
            if (state == AssetLinks.YES) return VERIFIED;
        }
        return "all".equals(mode) ? SELECTED : 0;
    }

    /* The domain at which the app is verified for links to `host`: the host as one of its filters with android:autoVerify
       names it ("*.example.com" at example.com); null when none does. */
    private static String verifiedDomain(Target t, String host) {
        for (Manifest.Filter f : filtersOf(t)) {
            if (!f.autoVerify || !IntentMatch.handlesWebUris(f, false)) continue;
            for (String v : f.hosts) {
                String h = Manifest.val(v);
                if (h == null) continue;
                if (h.startsWith("*.") ? host.toLowerCase(java.util.Locale.ROOT).endsWith(h.substring(1).toLowerCase(java.util.Locale.ROOT)) : h.equalsIgnoreCase(host))
                    return h.startsWith("*.") ? h.substring(2) : h;
            }
        }
        return null;
    }

    /** Every filter of the app's activities. */
    static List<Manifest.Filter> filtersOf(Target t) {
        ArrayList<Manifest.Filter> l = new ArrayList<>();
        if (t.app != null) {
            List<PhoneApps.Activity> acts = PhoneApps.activities(t.app);
            if (acts != null) for (PhoneApps.Activity x : acts) l.addAll(x.filters());
        } else if (t.own != null) {
            for (Manifest.Component c : Manifest.activities) l.addAll(c.filters);
        }
        return l;
    }

    /* The certificates the app's APK is signed with (each its DER), as the shell described them. */
    private static List<byte[]> certificates(Target t) {
        ArrayList<byte[]> l = new ArrayList<>();
        android.content.pm.Signature[] s = null;
        if (t.app != null) s = PhoneApps.signatures(t.app);
        else {
            byte[] json = Native.appActivities(Native.packageName());
            if (json != null) {
                try {
                    org.json.JSONArray certs = new org.json.JSONObject(new String(json, java.nio.charset.StandardCharsets.UTF_8)).optJSONArray("certificates");
                    if (certs != null) {
                        s = new android.content.pm.Signature[certs.length()];
                        for (int k = 0; k < certs.length(); k++) s[k] = new android.content.pm.Signature(certs.getString(k));
                    }
                } catch (org.json.JSONException e) {}
            }
        }
        if (s != null) for (android.content.pm.Signature x : s) l.add(x.toByteArray());
        return l;
    }

    /** The browser web links open in: the user's default among `browsers`, else Windows' own. */
    static Target defaultBrowser(List<Target> browsers) {
        String id = Defaults.get(Defaults.BROWSER);
        if (id != null && !id.isEmpty() && !Defaults.WINDOWS.equals(id)) {
            for (Target t : browsers) if (t.id().equals(id)) return t;
            String pkg = id.indexOf('/') > 0 ? id.substring(0, id.indexOf('/')) : id;
            for (Target t : browsers) if (pkg.equals(t.pkg)) return t;
        }
        return WINDOWS;
    }

    /**
     * The kind of intent an "Always" is for, as Android keeps a preferred activity for an intent's filter: its action, its
     * data's scheme, its type, and for a web link its host -- "android.intent.action.VIEW|geo|", "...VIEW|https|example.org".
     */
    static String kind(Intent i) {
        android.net.Uri d = i.getData();
        String scheme = d != null && d.getScheme() != null ? d.getScheme() : "", type = i.getType() != null ? i.getType() : "";
        StringBuilder b = new StringBuilder(i.getAction()).append('|').append(scheme).append('|').append(type);
        if (IntentMatch.isWebIntent(i) && d.getHost() != null) b.append('|').append(d.getHost().toLowerCase(java.util.Locale.ROOT));
        return b.toString().replace('=', '_').replace('\n', ' ');
    }

    /** The link as the log may say it: its scheme and host, no path, no query (which may carry a code or a token). */
    static String said(Intent i) {
        android.net.Uri d = i.getData();
        if (d == null) return "data of type " + i.getType();
        String host = d.getHost();
        return d.getScheme() + (host != null ? "://" + host : ":") + (i.getType() != null ? " (" + i.getType() + ")" : "");
    }

    // ---- startActivity

    /**
     * Context.startActivity's way for an intent this applies to: started, "Open with" shown, or decided off the main thread
     * while a domain is asked about; true then. False when nothing takes it, and AppRunner goes on as before.
     */
    public static boolean start(Context from, Intent i, int request) {
        if (!applies(i)) return false;
        long started = android.os.SystemClock.uptimeMillis();
        Decision d = decide(i, false);
        if (d.unknown) {
            final Intent intent = new Intent(i);
            new Thread(() -> {
                Decision late = decide(intent, true);
                main(() -> go(from, intent, request, late));
            }, "Megler links").start();
            return true;
        }
        if (d.targets.isEmpty()) return false;
        long took = android.os.SystemClock.uptimeMillis() - started;
        if (took > 300) android.util.Log.i("Megler", "links: resolving " + said(i) + " took " + took + " ms");
        // ("Open with" is a window: on the main thread, whichever thread the app started it from.)
        main(() -> go(from, i, request, d));
        return true;
    }

    private static void main(Runnable r) {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) r.run();
        else new android.os.Handler(android.os.Looper.getMainLooper()).post(r);
    }

    private static void go(Context from, Intent i, int request, Decision d) {
        if (d.targets.size() == 1) {
            Target t = d.targets.get(0);
            android.util.Log.i("Megler", "links: " + said(i) + " -> " + describe(t) + ": " + d.why);
            launch(from, i, request, t);
            return;
        }
        Activity over = from instanceof Activity && !((Activity) from).isFinishing() ? (Activity) from : AppRunner.top();
        StringBuilder names = new StringBuilder();
        for (Target t : d.targets) names.append(names.length() > 0 ? ", " : "").append(describe(t));
        if (over == null) {
            android.util.Log.i("Megler", "links: " + said(i) + " -> " + describe(d.targets.get(0)) + ", the first of " + names + ": no screen to ask on");
            launch(from, i, request, d.targets.get(0));
            return;
        }
        android.util.Log.i("Megler", "links: " + said(i) + " -> Open with: " + names + " (" + d.why + ")");
        final String kind = kind(i);
        Chooser.show(over, d.targets, true, (t, always) -> {
            if (always) {
                Defaults.set(Defaults.ALWAYS + kind, t.id());
                android.util.Log.i("Megler", "links: \"Always\" " + describe(t) + " for " + kind);
            }
            android.util.Log.i("Megler", "links: " + said(i) + " -> " + describe(t) + ", the user's choice");
            launch(from, i, request, t);
        }, () -> {
            android.util.Log.i("Megler", "links: " + said(i) + " -> Open with was closed, nothing opened");
            if (request >= 0 && from instanceof Activity) ((Activity) from).meglerActivityResult(request, Activity.RESULT_CANCELED, null);
        });
    }

    private static String describe(Target t) {
        return t.windows() ? "Windows' browser" : (t.own != null ? "this app's " : "") + t.pkg + "/" + t.cls.substring(t.cls.lastIndexOf('.') + 1);
    }

    /** The intent to the target: this app's activity (AppRunner), another app's (the shell), or Windows. */
    static void launch(Context from, Intent i, int request, Target t) {
        if (t.windows()) {
            String s = i.getData() != null ? i.getData().toString() : "";
            if (s.startsWith("market://")) s = "https://play.google.com/store/apps/" + s.substring(9);
            Native.openUrl(s);
            return;
        }
        Intent x = new Intent(i).setComponent(new ComponentName(t.pkg, t.cls));
        if (t.own != null) {
            AppRunner.startOwn(from, x, request, t.own);
            return;
        }
        PhoneApps.startWith(from, x, request);
    }

    // ---- PackageManager

    /**
     * queryIntentActivities for an intent this applies to: those startActivity would choose among first (the one it opens,
     * where it opens one), then, unless the flags ask only for those (MATCH_DEFAULT_ONLY without MATCH_ALL), the others whose
     * filters take it; Windows' browser for a web link or a mailto: address, as "com.android.browser". No domain is asked
     * about from here: what was verified before counts.
     */
    public static List<ResolveInfo> query(Intent i, int flags) {
        boolean defaultOnly = (flags & 0x00010000) != 0, matchAll = (flags & 0x00020000) != 0;
        ArrayList<ResolveInfo> l = new ArrayList<>();
        ArrayList<Target> seen = new ArrayList<>();
        boolean web = IntentMatch.isWebIntent(i);
        // A web link resolved for an app to open: the apps approved for its domain, else the default browser, and no other.
        if (web && !matchAll && IntentMatch.isDomainVerificationIntent(i, defaultOnly)) {
            for (Target t : decide(i, false).targets) add(l, seen, t);
            return l;
        }
        // (The one startActivity would open first, where it would open one: resolveActivity's answer, whatever its flags -- an
        // app that asks whether a link has a browser is answered with the browser, not with its own unverified filter.)
        Decision d = decide(i, false);
        if (d.targets.size() == 1) add(l, seen, d.targets.get(0));
        for (Target t : candidates(i, IntentMatch.typeOf(i), defaultOnly)) add(l, seen, t);
        android.net.Uri data = i.getData();
        if (web || (data != null && "mailto".equals(data.getScheme()))) add(l, seen, WINDOWS);
        return l;
    }

    private static void add(List<ResolveInfo> l, List<Target> seen, Target t) {
        for (Target x : seen) if (x.id().equals(t.id())) return;
        seen.add(t);
        l.add(t.resolveInfo());
    }

    // ---- what Android's Settings and Megler's show of it (megler.SystemScreens, Megler's Settings)

    /** Every browser the phone has, Windows' first: the activities that take web links of any host. */
    public static List<Target> browsers() {
        ArrayList<Target> l = new ArrayList<>();
        l.add(WINDOWS);
        Intent probe = new Intent(Intent.ACTION_VIEW, android.net.Uri.parse("https://example.com/")).addCategory(Intent.CATEGORY_BROWSABLE);
        for (Target t : candidates(probe, null, true)) if (t.browser()) l.add(t);
        return l;
    }

    /** The web domains the app's filters name for links (VIEW, BROWSABLE, http or https), each once, with whether a filter
     *  with android:autoVerify names it. */
    public static java.util.LinkedHashMap<String, Boolean> domains(List<Manifest.Filter> filters) {
        java.util.LinkedHashMap<String, Boolean> out = new java.util.LinkedHashMap<>();
        for (Manifest.Filter f : filters) {
            if (!IntentMatch.handlesWebUris(f, false)) continue;
            for (String v : f.hosts) {
                String h = Manifest.val(v);
                if (h == null || h.isEmpty()) continue;
                Boolean had = out.get(h);
                out.put(h, (had != null && had) || f.autoVerify);
            }
        }
        return out;
    }

    /** Every filter of the app's own activities. */
    public static List<Manifest.Filter> ownFilters() {
        Manifest.read();
        ArrayList<Manifest.Filter> l = new ArrayList<>();
        for (Manifest.Component c : Manifest.activities) l.addAll(c.filters);
        return l;
    }

    /** A kind of intent (kind()) as the user reads it: "Links to example.org", "geo: addresses", "image/png files, EDIT". */
    public static String kindInWords(String kind) {
        String[] f = kind.split("\\|", -1);
        String action = f[0], scheme = f.length > 1 ? f[1] : "", type = f.length > 2 ? f[2] : "", host = f.length > 3 ? f[3] : "";
        String what = !host.isEmpty() ? "Links to " + host : !scheme.isEmpty() ? scheme + ": addresses" + (type.isEmpty() ? "" : " (" + type + ")")
                    : !type.isEmpty() ? type + " files" : "Others";
        String a = action.startsWith("android.intent.action.") ? action.substring("android.intent.action.".length()) : action;
        return "VIEW".equals(a) ? what : what + ", " + a;
    }

    /** The app a kept answer ("<package>/<activity>", "windows") names, by its label. */
    public static String labelOf(String id) {
        if (id == null || Defaults.WINDOWS.equals(id)) return "Windows";
        String pkg = id.indexOf('/') > 0 ? id.substring(0, id.indexOf('/')) : id;
        PhoneApps.App a = PhoneApps.find(pkg);
        if (a != null) return a.label;
        if (pkg.equals(Native.packageName())) {
            try { return String.valueOf(ContextImpl.app().getApplicationInfo().loadLabel(ContextImpl.app().getPackageManager())); } catch (RuntimeException e) {}
        }
        return pkg;
    }

    /** Whether the app is verified for the domain, as last found (AssetLinks): true, false, or null when not known. */
    public static Boolean verified(String pkg, String domain) {
        int s = AssetLinks.cached(pkg, domain.startsWith("*.") ? domain.substring(2) : domain);
        return s == AssetLinks.YES ? Boolean.TRUE : s == AssetLinks.NO ? Boolean.FALSE : null;
    }
}
