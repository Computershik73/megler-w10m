// Megler: Android's, the configuration in an app's XML resource (NetworkSecurityConfigProvider.java says why this package is
// here). It differs where the file breaks Android's rules -- an element where Android allows none, a domain named twice, a
// pin of the wrong length: Android throws, which ends the app at its first connection; here it is said in the log and the
// platform's default is used. Attribute values are read as their text ("true", a date), which the compiled file gives both
// for a value it typed and for one it kept as a string. And what was read is said in the log once.
package android.security.net.config;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.Base64;
import android.util.Log;
import android.util.Pair;
import com.android.internal.util.XmlUtils;
import java.io.IOException;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Date;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/**
 * <network-security-config>: at most one <base-config>, which falls back on the platform's default; <domain-config>s, each
 * with its <domain>s and maybe <domain-config>s inside, which fall back on the one around them and then on the base one; and
 * <debug-overrides> (or those of the resource named as this one with "_debug" after it), whose anchors are added to every
 * configuration that has anchors of its own, for a debuggable app alone.
 */
public class XmlConfigSource implements ConfigSource {
    private static final String TAG = "NetworkSecurityConfig";
    private static final int CONFIG_BASE = 0, CONFIG_DOMAIN = 1, CONFIG_DEBUG = 2;

    private final Object mLock = new Object();
    private final int mResourceId;
    private final boolean mDebugBuild;
    private final ApplicationInfo mApplicationInfo;
    private Context mContext;
    private boolean mInitialized;
    private NetworkSecurityConfig mDefaultConfig;
    private Set<Pair<Domain, NetworkSecurityConfig>> mDomainMap;

    public XmlConfigSource(Context context, int resourceId, ApplicationInfo info) {
        mContext = context;
        mResourceId = resourceId;
        mApplicationInfo = new ApplicationInfo(info);
        mDebugBuild = (mApplicationInfo.flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    @Override public Set<Pair<Domain, NetworkSecurityConfig>> getPerDomainConfigs() {
        ensureInitialized();
        return mDomainMap;
    }

    @Override public NetworkSecurityConfig getDefaultConfig() {
        ensureInitialized();
        return mDefaultConfig;
    }

    private static String getConfigString(int configType) {
        switch (configType) {
            case CONFIG_BASE: return "base-config";
            case CONFIG_DOMAIN: return "domain-config";
            case CONFIG_DEBUG: return "debug-overrides";
            default: throw new IllegalArgumentException("Unknown config type: " + configType);
        }
    }

    private void ensureInitialized() {
        synchronized (mLock) {
            if (mInitialized) return;
            String name = name(mContext, mResourceId);
            XmlResourceParser parser = null;
            boolean read = false;
            try {
                parser = mContext.getResources().getXml(mResourceId);
                parseNetworkSecurityConfig(parser);
                read = true;
            } catch (Exception e) {
                Log.w(TAG, "the app's network security configuration " + name + " cannot be read, and the platform's default is used: " + e);
                mDefaultConfig = NetworkSecurityConfig.getDefaultBuilder(mApplicationInfo).build();
                mDomainMap = null;
            } finally {
                if (parser != null) parser.close();
            }
            mContext = null;
            mInitialized = true;
            if (read) {
                try {
                    Log.i(TAG, "the app's network security configuration " + name + (mDebugBuild ? ", a debuggable app's" : "") + ": " + describe());
                } catch (RuntimeException e) {
                    Log.w(TAG, "the app's network security configuration " + name + " was read; saying what it is failed: " + e);
                }
            }
        }
    }

    static String name(Context context, int id) {
        try {
            Resources r = context.getResources();
            return r.getResourceTypeName(id) + "/" + r.getResourceEntryName(id);
        } catch (Exception e) {
            return "0x" + Integer.toHexString(id);
        }
    }

    /** What was read, for the log: the base configuration, then each domain configuration with its domains. */
    private String describe() {
        StringBuilder s = new StringBuilder("base-config: ").append(mDefaultConfig.describe());
        Map<NetworkSecurityConfig, List<Domain>> byConfig = new IdentityHashMap<>();
        List<NetworkSecurityConfig> order = new ArrayList<>();
        for (Pair<Domain, NetworkSecurityConfig> entry : mDomainMap) {
            List<Domain> domains = byConfig.get(entry.second);
            if (domains == null) {
                byConfig.put(entry.second, domains = new ArrayList<>());
                order.add(entry.second);
            }
            domains.add(entry.first);
        }
        for (NetworkSecurityConfig config : order) {
            s.append("; domain-config of");
            for (Domain d : byConfig.get(config)) s.append(' ').append(d.hostname).append(d.subdomainsIncluded ? " (and subdomains)" : "");
            s.append(": ").append(config.describe());
        }
        return s.toString();
    }

    private void parseNetworkSecurityConfig(XmlResourceParser parser) throws IOException, XmlPullParserException, ParserException {
        Set<String> seenDomains = new HashSet<>();
        List<Pair<NetworkSecurityConfig.Builder, Set<Domain>>> builders = new ArrayList<>();
        NetworkSecurityConfig.Builder baseConfigBuilder = null;
        NetworkSecurityConfig.Builder debugConfigBuilder = null;
        boolean seenDebugOverrides = false, seenBaseConfig = false;

        XmlUtils.beginDocument(parser, "network-security-config");
        int outerDepth = parser.getDepth();
        while (XmlUtils.nextElementWithin(parser, outerDepth)) {
            String tag = parser.getName();
            if ("base-config".equals(tag)) {
                if (seenBaseConfig) throw new ParserException(parser, "Only one base-config allowed");
                seenBaseConfig = true;
                baseConfigBuilder = parseConfigEntry(parser, seenDomains, null, CONFIG_BASE).get(0).first;
            } else if ("domain-config".equals(tag)) {
                builders.addAll(parseConfigEntry(parser, seenDomains, baseConfigBuilder, CONFIG_DOMAIN));
            } else if ("debug-overrides".equals(tag)) {
                if (seenDebugOverrides) throw new ParserException(parser, "Only one debug-overrides allowed");
                if (mDebugBuild) debugConfigBuilder = parseConfigEntry(parser, null, null, CONFIG_DEBUG).get(0).first;
                else XmlUtils.skipCurrentTag(parser);
                seenDebugOverrides = true;
            } else {
                XmlUtils.skipCurrentTag(parser);
            }
        }
        if (mDebugBuild && debugConfigBuilder == null) debugConfigBuilder = parseDebugOverridesResource();

        // The platform's default is the parent of the base configuration, or the base configuration itself when there is none.
        NetworkSecurityConfig.Builder platformDefaultBuilder = NetworkSecurityConfig.getDefaultBuilder(mApplicationInfo);
        addDebugAnchorsIfNeeded(debugConfigBuilder, platformDefaultBuilder);
        if (baseConfigBuilder != null) {
            baseConfigBuilder.setParent(platformDefaultBuilder);
            addDebugAnchorsIfNeeded(debugConfigBuilder, baseConfigBuilder);
        } else {
            baseConfigBuilder = platformDefaultBuilder;
        }
        // A domain configuration read before the base one has no parent yet. Parents come before their children in the list.
        Set<Pair<Domain, NetworkSecurityConfig>> configs = new LinkedHashSet<>();
        for (Pair<NetworkSecurityConfig.Builder, Set<Domain>> entry : builders) {
            NetworkSecurityConfig.Builder builder = entry.first;
            if (builder.getParent() == null) builder.setParent(baseConfigBuilder);
            addDebugAnchorsIfNeeded(debugConfigBuilder, builder);
            NetworkSecurityConfig config = builder.build();
            for (Domain domain : entry.second) configs.add(new Pair<>(domain, config));
        }
        mDefaultConfig = baseConfigBuilder.build();
        mDomainMap = configs;
    }

    /** The debug anchors go to a configuration with anchors of its own; one without takes its parent's, which have them. */
    private static void addDebugAnchorsIfNeeded(NetworkSecurityConfig.Builder debugConfigBuilder, NetworkSecurityConfig.Builder builder) {
        if (debugConfigBuilder == null || !debugConfigBuilder.hasCertificatesEntryRefs()) return;
        if (!builder.hasCertificatesEntryRefs()) return;
        builder.addCertificatesEntryRefs(debugConfigBuilder.getCertificatesEntryRefs());
    }

    private NetworkSecurityConfig.Builder parseDebugOverridesResource() throws IOException, XmlPullParserException, ParserException {
        Resources resources = mContext.getResources();
        int resId;
        try {
            resId = resources.getIdentifier(resources.getResourceEntryName(mResourceId) + "_debug", "xml", resources.getResourcePackageName(mResourceId));
        } catch (RuntimeException e) {
            return null;
        }
        if (resId == 0) return null;
        NetworkSecurityConfig.Builder debugConfigBuilder = null;
        XmlResourceParser parser = resources.getXml(resId);
        try {
            XmlUtils.beginDocument(parser, "network-security-config");
            int outerDepth = parser.getDepth();
            boolean seenDebugOverrides = false;
            while (XmlUtils.nextElementWithin(parser, outerDepth)) {
                if ("debug-overrides".equals(parser.getName())) {
                    if (seenDebugOverrides) throw new ParserException(parser, "Only one debug-overrides allowed");
                    debugConfigBuilder = parseConfigEntry(parser, null, null, CONFIG_DEBUG).get(0).first;
                    seenDebugOverrides = true;
                } else {
                    XmlUtils.skipCurrentTag(parser);
                }
            }
        } finally {
            parser.close();
        }
        return debugConfigBuilder;
    }

    /** A configuration and those inside it, each with its domains, the parser on its start tag; the outer one first. */
    private List<Pair<NetworkSecurityConfig.Builder, Set<Domain>>> parseConfigEntry(XmlResourceParser parser, Set<String> seenDomains,
            NetworkSecurityConfig.Builder parentBuilder, int configType) throws IOException, XmlPullParserException, ParserException {
        List<Pair<NetworkSecurityConfig.Builder, Set<Domain>>> builders = new ArrayList<>();
        NetworkSecurityConfig.Builder builder = new NetworkSecurityConfig.Builder();
        builder.setParent(parentBuilder);
        Set<Domain> domains = new LinkedHashSet<>();
        boolean seenPinSet = false, seenTrustAnchors = false;
        boolean defaultOverridePins = configType == CONFIG_DEBUG;
        int outerDepth = parser.getDepth();
        builders.add(new Pair<>(builder, domains));
        // only what is said is set: the rest comes from the parent
        for (int i = 0; i < parser.getAttributeCount(); i++) {
            String name = parser.getAttributeName(i);
            if ("hstsEnforced".equals(name)) builder.setHstsEnforced(bool(parser.getAttributeValue(i), NetworkSecurityConfig.DEFAULT_HSTS_ENFORCED));
            else if ("cleartextTrafficPermitted".equals(name))
                builder.setCleartextTrafficPermitted(bool(parser.getAttributeValue(i), NetworkSecurityConfig.DEFAULT_CLEARTEXT_TRAFFIC_PERMITTED));
        }
        while (XmlUtils.nextElementWithin(parser, outerDepth)) {
            String tagName = parser.getName();
            if ("domain".equals(tagName)) {
                if (configType != CONFIG_DOMAIN) throw new ParserException(parser, "domain element not allowed in " + getConfigString(configType));
                domains.add(parseDomain(parser, seenDomains));
            } else if ("trust-anchors".equals(tagName)) {
                if (seenTrustAnchors) throw new ParserException(parser, "Multiple trust-anchor elements not allowed");
                builder.addCertificatesEntryRefs(parseTrustAnchors(parser, defaultOverridePins));
                seenTrustAnchors = true;
            } else if ("pin-set".equals(tagName)) {
                if (configType != CONFIG_DOMAIN) throw new ParserException(parser, "pin-set element not allowed in " + getConfigString(configType));
                if (seenPinSet) throw new ParserException(parser, "Multiple pin-set elements not allowed");
                builder.setPinSet(parsePinSet(parser));
                seenPinSet = true;
            } else if ("domain-config".equals(tagName)) {
                if (configType != CONFIG_DOMAIN) throw new ParserException(parser, "Nested domain-config not allowed in " + getConfigString(configType));
                builders.addAll(parseConfigEntry(parser, seenDomains, builder, configType));
            } else {
                XmlUtils.skipCurrentTag(parser);
            }
        }
        if (configType == CONFIG_DOMAIN && domains.isEmpty()) throw new ParserException(parser, "No domain elements in domain-config");
        return builders;
    }

    private Domain parseDomain(XmlResourceParser parser, Set<String> seenDomains) throws IOException, XmlPullParserException, ParserException {
        boolean includeSubdomains = bool(parser.getAttributeValue(null, "includeSubdomains"), false);
        String text = text(parser);
        if (text == null) throw new ParserException(parser, "Domain name missing");
        String domain = text.trim().toLowerCase(Locale.US);
        // the most specific domain is chosen, so a name may be given once, with includeSubdomains or without
        if (!seenDomains.add(domain)) throw new ParserException(parser, domain + " has already been specified");
        return new Domain(domain, includeSubdomains);
    }

    private PinSet parsePinSet(XmlResourceParser parser) throws IOException, XmlPullParserException, ParserException {
        String expirationDate = parser.getAttributeValue(null, "expiration");
        long expiration = Long.MAX_VALUE;
        if (expirationDate != null) {
            try {
                SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
                sdf.setLenient(false);
                Date date = sdf.parse(expirationDate.trim());
                if (date == null) throw new ParserException(parser, "Invalid expiration date in pin-set");
                expiration = date.getTime();
            } catch (ParseException e) {
                throw new ParserException(parser, "Invalid expiration date in pin-set", e);
            }
        }
        int outerDepth = parser.getDepth();
        Set<Pin> pins = new HashSet<>();
        while (XmlUtils.nextElementWithin(parser, outerDepth)) {
            if ("pin".equals(parser.getName())) pins.add(parsePin(parser));
            else XmlUtils.skipCurrentTag(parser);
        }
        return new PinSet(pins, expiration);
    }

    private Pin parsePin(XmlResourceParser parser) throws IOException, XmlPullParserException, ParserException {
        String digestAlgorithm = parser.getAttributeValue(null, "digest");
        if (!Pin.isSupportedDigestAlgorithm(digestAlgorithm)) throw new ParserException(parser, "Unsupported pin digest algorithm: " + digestAlgorithm);
        String digest = text(parser);
        if (digest == null) throw new ParserException(parser, "Missing pin digest");
        byte[] decoded;
        try {
            decoded = Base64.decode(digest.trim(), 0);
        } catch (IllegalArgumentException e) {
            throw new ParserException(parser, "Invalid pin digest", e);
        }
        int expected = Pin.getDigestLength(digestAlgorithm);
        if (decoded.length != expected)
            throw new ParserException(parser, "digest length " + decoded.length + " does not match expected length for " + digestAlgorithm + " of " + expected);
        return new Pin(digestAlgorithm, decoded);
    }

    private Collection<CertificatesEntryRef> parseTrustAnchors(XmlResourceParser parser, boolean defaultOverridePins)
            throws IOException, XmlPullParserException, ParserException {
        int outerDepth = parser.getDepth();
        List<CertificatesEntryRef> anchors = new ArrayList<>();
        while (XmlUtils.nextElementWithin(parser, outerDepth)) {
            if ("certificates".equals(parser.getName())) anchors.add(parseCertificatesEntry(parser, defaultOverridePins));
            else XmlUtils.skipCurrentTag(parser);
        }
        return anchors;
    }

    private CertificatesEntryRef parseCertificatesEntry(XmlResourceParser parser, boolean defaultOverridePins)
            throws IOException, XmlPullParserException, ParserException {
        boolean overridePins = bool(parser.getAttributeValue(null, "overridePins"), defaultOverridePins);
        int sourceId = parser.getAttributeResourceValue(null, "src", -1);
        String sourceString = parser.getAttributeValue(null, "src");
        if (sourceString == null) throw new ParserException(parser, "certificates element missing src attribute");
        CertificateSource source;
        if (sourceId != -1) source = new ResourceCertificateSource(sourceId, mContext);
        else if ("system".equals(sourceString)) source = SystemCertificateSource.getInstance();
        else if ("user".equals(sourceString)) source = UserCertificateSource.getInstance();
        else throw new ParserException(parser, "Unknown certificates src. Should be one of system|user|@resourceVal");
        XmlUtils.skipCurrentTag(parser);
        return new CertificatesEntryRef(source, overridePins);
    }

    /** A boolean attribute by its text, "true" or "false"; anything else is the default, as Android's parser gives it. */
    private static boolean bool(String value, boolean def) {
        if (value == null) return def;
        value = value.trim();
        if ("true".equalsIgnoreCase(value)) return true;
        if ("false".equalsIgnoreCase(value)) return false;
        return def;
    }

    /** An element's text, the parser left on its end tag; null when it has none. An element inside it is an error, as on Android. */
    private static String text(XmlResourceParser parser) throws IOException, XmlPullParserException, ParserException {
        int depth = parser.getDepth();
        String element = parser.getName();
        StringBuilder s = null;
        int t;
        while ((t = parser.next()) != XmlPullParser.END_DOCUMENT) {
            if (t == XmlPullParser.END_TAG && parser.getDepth() == depth) break;
            if (t == XmlPullParser.START_TAG) throw new ParserException(parser, element + " contains additional elements");
            if (t == XmlPullParser.TEXT) {
                if (s == null) s = new StringBuilder();
                s.append(parser.getText());
            }
        }
        return s == null ? null : s.toString();
    }

    /** What breaks Android's rules for the file, and where. */
    public static class ParserException extends Exception {
        public ParserException(XmlPullParser parser, String message, Throwable cause) {
            super(message + " at: " + parser.getPositionDescription(), cause);
        }

        public ParserException(XmlPullParser parser, String message) {
            this(parser, message, null);
        }
    }
}
