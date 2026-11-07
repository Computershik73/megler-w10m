// Megler: Android's, the configuration an app's manifest gives it (NetworkSecurityConfigProvider.java says why this package is
// here). It differs in reading the two attributes of <application> from the manifest itself: Android's package manager puts
// them in the ApplicationInfo (networkSecurityConfigRes, FLAG_USES_CLEARTEXT_TRAFFIC), which Megler's does not fill in.
package android.security.net.config;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.XmlBlock;
import android.util.Log;
import android.util.Pair;
import java.util.Set;
import org.xmlpull.v1.XmlPullParser;

/**
 * The app's configuration, made at its first use: from the XML resource its manifest names (android:networkSecurityConfig),
 * else the platform's default with cleartext as android:usesCleartextTraffic says -- permitted, unless the app is written for
 * Android 9 or later (targetSdkVersion 28 and above) and does not say so.
 */
public class ManifestConfigSource implements ConfigSource {
    private static final String TAG = "NetworkSecurityConfig";
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private final Object mLock = new Object();
    private final Context mContext;
    private final ApplicationInfo mApplicationInfo;
    private ConfigSource mConfigSource;

    public ManifestConfigSource(Context context) {
        mContext = context;
        mApplicationInfo = new ApplicationInfo(context.getApplicationInfo());
    }

    @Override public Set<Pair<Domain, NetworkSecurityConfig>> getPerDomainConfigs() {
        return getConfigSource().getPerDomainConfigs();
    }

    @Override public NetworkSecurityConfig getDefaultConfig() {
        return getConfigSource().getDefaultConfig();
    }

    private ConfigSource getConfigSource() {
        synchronized (mLock) {
            if (mConfigSource != null) return mConfigSource;
            boolean byDefault = mApplicationInfo.targetSdkVersion < 28;
            Boolean uses = null;
            int config = 0;
            try {
                byte[] d = megler.Native.readApkFile(0, "AndroidManifest.xml");
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
                Log.w(TAG, "the manifest cannot be read: " + e);
            }
            mApplicationInfo.networkSecurityConfigRes = config;
            if (config != 0) {
                mConfigSource = new XmlConfigSource(mContext, config, mApplicationInfo);
            } else {
                boolean cleartext = uses != null ? uses : byDefault;
                mConfigSource = new DefaultConfigSource(cleartext, mApplicationInfo);
            }
            return mConfigSource;
        }
    }

    /** An app without a configuration: the platform's default, cleartext as its manifest says. */
    private static final class DefaultConfigSource implements ConfigSource {
        private final NetworkSecurityConfig mDefaultConfig;

        DefaultConfigSource(boolean usesCleartextTraffic, ApplicationInfo info) {
            mDefaultConfig = NetworkSecurityConfig.getDefaultBuilder(info).setCleartextTrafficPermitted(usesCleartextTraffic).build();
        }

        @Override public NetworkSecurityConfig getDefaultConfig() { return mDefaultConfig; }

        @Override public Set<Pair<Domain, NetworkSecurityConfig>> getPerDomainConfigs() { return null; }
    }
}
