// Overlay of core/framework/gen-src/android/security/NetworkSecurityPolicy.java (tools/build-framework.sh).
// The original's is a generated signature: cleartext traffic was never permitted, to any host. This one answers as Android
// does, from the app's manifest and its network security configuration:
//  - with android:networkSecurityConfig, that XML resource decides: its <base-config cleartextTrafficPermitted>, and for a
//    host, the most specific <domain-config> whose <domain> names it (includeSubdomains for its subdomains), nested ones
//    taking what they do not say from the one around them; what none says is the default below;
//  - without one, android:usesCleartextTraffic;
//  - without either, the default: permitted for an app written for Android 8.1 or earlier (targetSdkVersion 27 or less),
//    not for a later one.
// (Android ignores usesCleartextTraffic when there is a configuration; so does this. Certificates and pins in the
// configuration are not this class's: TLS is conscrypt's.)
//
// Why. Android's WebView asks this class for every http:// request (Chromium's AwContentBrowserClient, by host) and refuses
// the request with net::ERR_CLEARTEXT_NOT_PERMITTED when it says no. Every http page failed to load, in every app, whatever
// its manifest said.
package android.security;

import android.content.res.XmlBlock;
import org.xmlpull.v1.XmlPullParser;

import java.util.ArrayList;

public class NetworkSecurityPolicy {
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static NetworkSecurityPolicy sInstance;

    /** One <domain> of a <domain-config>, and what that configuration says (null: what the one around it says). */
    private static final class Domain {
        final String name;
        final boolean subdomains;
        final Boolean cleartext;
        Domain(String name, boolean subdomains, Boolean cleartext) { this.name = name; this.subdomains = subdomains; this.cleartext = cleartext; }
    }

    private volatile boolean mBase;
    private final ArrayList<Domain> mDomains = new ArrayList<>();

    protected NetworkSecurityPolicy() { read(); }

    public static synchronized NetworkSecurityPolicy getInstance() {
        if (sInstance == null) sInstance = new NetworkSecurityPolicy();
        return sInstance;
    }

    public boolean isCleartextTrafficPermitted() { return mBase; }

    public boolean isCleartextTrafficPermitted(String hostname) {
        if (hostname == null) return mBase;
        String host = hostname.toLowerCase(java.util.Locale.ROOT);
        Domain best = null;
        for (Domain d : mDomains) {
            boolean match = host.equals(d.name) || (d.subdomains && host.endsWith("." + d.name));
            // the most specific: an exact name before a parent's includeSubdomains, a longer name before a shorter one
            if (match && (best == null || d.name.length() > best.name.length() || (d.name.equals(best.name) && host.equals(d.name)))) best = d;
        }
        return best != null && best.cleartext != null ? best.cleartext : mBase;
    }

    public void setCleartextTrafficPermitted(boolean permitted) { mBase = permitted; }

    public boolean isCertificateTransparencyVerificationRequired(String hostname) { return false; }

    public void handleTrustStorageUpdate() {}

    public static android.security.net.config.ApplicationConfig getApplicationConfigForPackage(android.content.Context context, String packageName) { return null; }

    /** The manifest's word on it, and the configuration it names (see the top of the file). */
    private void read() {
        meglet.Manifest.read();
        boolean byDefault = meglet.Manifest.targetSdk < 28;
        Boolean uses = null;
        int config = 0;
        try {
            byte[] d = meglet.Native.readApkFile(0, "AndroidManifest.xml");
            if (d != null) {
                XmlBlock.Parser p = new XmlBlock(d, "AndroidManifest.xml").newParser();
                int t;
                while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                    if (t != XmlPullParser.START_TAG || !"application".equals(p.getName())) continue;
                    if (p.getAttributeValue(NS, "usesCleartextTraffic") != null) uses = p.getAttributeBooleanValue(NS, "usesCleartextTraffic", byDefault);
                    config = p.getAttributeResourceValue(NS, "networkSecurityConfig", 0);
                    break;
                }
            }
        } catch (Exception e) {
            android.util.Log.w("NetworkSecurityPolicy", "the manifest cannot be read: " + e);
        }
        mBase = uses != null ? uses : byDefault;
        if (config != 0) readConfig(config, byDefault);
    }

    private void readConfig(int id, boolean byDefault) {
        android.app.Application app = android.app.ActivityThread.currentApplication();
        if (app == null) return;
        mBase = byDefault;
        try {
            XmlPullParser p = app.getResources().getXml(id);
            // the cleartext setting of each open <domain-config>, innermost last (null: not said there)
            ArrayList<Boolean> open = new ArrayList<>();
            int t;
            while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                if (t == XmlPullParser.END_TAG && "domain-config".equals(p.getName()) && !open.isEmpty()) open.remove(open.size() - 1);
                if (t != XmlPullParser.START_TAG) continue;
                String tag = p.getName();
                String said = p.getAttributeValue(NS, "cleartextTrafficPermitted");
                if (said == null) said = p.getAttributeValue(null, "cleartextTrafficPermitted");
                if ("base-config".equals(tag)) {
                    if (said != null) mBase = Boolean.parseBoolean(said);
                } else if ("domain-config".equals(tag)) {
                    Boolean outer = open.isEmpty() ? null : open.get(open.size() - 1);
                    open.add(said != null ? Boolean.valueOf(said) : outer);
                } else if ("domain".equals(tag) && !open.isEmpty()) {
                    String sub = p.getAttributeValue(NS, "includeSubdomains");
                    if (sub == null) sub = p.getAttributeValue(null, "includeSubdomains");
                    String name = p.nextText();
                    if (name != null && !name.trim().isEmpty())
                        mDomains.add(new Domain(name.trim().toLowerCase(java.util.Locale.ROOT), Boolean.parseBoolean(sub), open.get(open.size() - 1)));
                    // nextText leaves the parser on </domain>, which the loop takes next
                }
            }
        } catch (Exception e) {
            android.util.Log.w("NetworkSecurityPolicy", "the network security configuration cannot be read: " + e);
        }
    }
}
