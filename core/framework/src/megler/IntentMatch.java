package megler;

import android.content.Intent;
import android.net.Uri;
import java.util.List;
import java.util.Set;

/**
 * Android's IntentFilter.match, for the filters of the app's manifest (megler.Manifest) and of the other apps' as the shell
 * described them (megler.PhoneApps): the action (an intent without one passes), the data -- the scheme, a scheme-specific
 * part, else the host with its port and then the path, the MIME type --, then the categories, every one of the intent's
 * being the filter's; and, for what startActivity resolves (PackageManager.MATCH_DEFAULT_ONLY), CATEGORY_DEFAULT among the
 * filter's. Paths and scheme-specific parts are matched as android.os.PatternMatcher matches them: literally, by prefix, by
 * suffix, by its simple glob (pathPattern, whose ".*" takes the least it can up to the next character) and by its advanced
 * glob.
 *
 * The data was the scheme and the host alone, and no path, no port and no type was looked at: Komi Store's filter for
 * https://github.com/<owner>/<repo> (pathPattern "/.*\/.*") took every link to github.com. And the web's links are not every
 * filter's that matches them (megler.Resolver): Android 12 and later open an http or https link in an app only where the app
 * is verified for its domain or the user allowed it, and in the browser otherwise; isDomainVerificationIntent and
 * handlesAllWebData say which intents and which filters that is about.
 */
public final class IntentMatch {
    private IntentMatch() {}

    /** android.os.PatternMatcher's kinds: PATTERN_LITERAL, PATTERN_PREFIX, PATTERN_SIMPLE_GLOB, PATTERN_ADVANCED_GLOB, PATTERN_SUFFIX. */
    public static final int LITERAL = 0, PREFIX = 1, GLOB = 2, ADVANCED = 3, SUFFIX = 4;

    /** A filter's path or scheme-specific part: its kind and its text (a string resource's, "@<id>", read when used). */
    public static final class Part {
        public final int kind;
        public final String value;
        public Part(int kind, String value) { this.kind = kind; this.value = value; }
        public String kindName() { return kindNames[kind]; }
    }

    private static final String[] kindNames = { "literal", "prefix", "glob", "advanced", "suffix" };

    /** The kind the shell's description names ("literal", "prefix", "glob", "advanced", "suffix"); -1 for another. */
    public static int kindOf(String name) {
        for (int k = 0; k < kindNames.length; k++) if (kindNames[k].equals(name)) return k;
        return -1;
    }

    /** The manifest's attributes of a <data> for a path and a scheme-specific part, each with its kind. */
    static final String[] PATH_ATTRS = { "path", "pathPrefix", "pathPattern", "pathAdvancedPattern", "pathSuffix" };
    static final String[] SSP_ATTRS = { "ssp", "sspPrefix", "sspPattern", "sspAdvancedPattern", "sspSuffix" };

    /**
     * Whether `f` takes `i`, whose MIME type is `type` (the intent's own, or what its content: data is, Intent.resolveType),
     * as IntentFilter.match has it; with `defaultOnly` the filter must have CATEGORY_DEFAULT as well, as an activity started
     * by an implicit intent must (MATCH_DEFAULT_ONLY).
     */
    public static boolean matches(Manifest.Filter f, Intent i, String type, boolean defaultOnly) {
        String action = i.getAction();
        if (action != null && !f.actions.contains(action)) return false;
        if (!matchData(f, type, i.getData())) return false;
        Set<String> categories = i.getCategories();
        if (categories != null) for (String c : categories) if (!has(f.categories, c)) return false;
        return !defaultOnly || has(f.categories, Intent.CATEGORY_DEFAULT);
    }

    /** The intent's MIME type as Android resolves it for matching: its own, else, for content: data, what the provider says. */
    public static String typeOf(Intent i) {
        String type = i.getType();
        Uri d = i.getData();
        if (type != null || d == null || !"content".equals(d.getScheme())) return type;
        try {
            android.content.Context app = ContextImpl.app();
            return app != null ? app.getContentResolver().getType(d) : null;
        } catch (Throwable t) {
            return null;
        }
    }

    /** IntentFilter.matchData. */
    static boolean matchData(Manifest.Filter f, String type, Uri data) {
        String scheme = data != null ? data.getScheme() : null;
        boolean types = !f.mimeTypes.isEmpty(), schemes = !f.schemes.isEmpty();
        if (!types && !schemes) return type == null && data == null;
        if (schemes) {
            if (!has(f.schemes, scheme != null ? scheme : "")) return false;
            boolean ssp = false;
            if (!f.ssps.isEmpty() && data != null) {
                ssp = matchesAny(f.ssps, data.getSchemeSpecificPart());
                if (!ssp && f.hosts.isEmpty()) return false;
            }
            // (No scheme-specific part matched: the host must, where the filter names hosts, and then its path.)
            if (!ssp && !f.hosts.isEmpty()) {
                if (!matchAuthority(f, data)) return false;
                if (!f.paths.isEmpty() && !matchesAny(f.paths, data.getPath())) return false;
            }
        } else if (scheme != null && !scheme.isEmpty() && !"content".equals(scheme) && !"file".equals(scheme)) {
            // A filter of types alone takes data by type: content: and file: data, and no other scheme.
            return false;
        }
        if (types) return findMimeType(f.mimeTypes, type);
        return type == null;
    }

    /** IntentFilter.matchDataAuthority: one of the filter's hosts, with its port where it has one. */
    static boolean matchAuthority(Manifest.Filter f, Uri data) {
        if (data == null) return false;
        String host = data.getHost();
        for (int k = 0; k < f.hosts.size(); k++) {
            String want = Manifest.val(f.hosts.get(k));
            if (want == null) continue;
            boolean wild = want.startsWith("*");
            if (wild) want = want.substring(1);
            if (host == null) {
                if (!(wild && want.isEmpty())) continue;
            } else {
                String h = host;
                if (wild) {
                    if (h.length() < want.length()) continue;
                    h = h.substring(h.length() - want.length());
                }
                if (!h.equalsIgnoreCase(want)) continue;
            }
            int port = k < f.ports.size() ? f.ports.get(k) : -1;
            if (port >= 0 && port != data.getPort()) continue;
            return true;
        }
        return false;
    }

    static boolean matchesAny(List<Part> parts, String s) {
        if (s == null) return false;
        for (Part p : parts) if (matches(p, s)) return true;
        return false;
    }

    /** PatternMatcher.match. */
    public static boolean matches(Part p, String s) {
        String pattern = Manifest.val(p.value);
        if (pattern == null || s == null) return false;
        switch (p.kind) {
            case LITERAL: return pattern.equals(s);
            case PREFIX: return s.startsWith(pattern);
            case SUFFIX: return s.endsWith(pattern);
            case GLOB: return glob(pattern, s);
            case ADVANCED: return advanced(pattern, s);
            default: return false;
        }
    }

    /*
     * PatternMatcher's simple glob: '.' any character, '*' as many of the character before it as there are (none too), "\"
     * makes the next one plain; ".*" takes characters up to the first of the one after it, as Android's does (not the most
     * it could), and at the pattern's end takes the rest.
     */
    static boolean glob(String pattern, String match) {
        final int np = pattern.length(), nm = match.length();
        if (np <= 0) return nm <= 0;
        int ip = 0, im = 0;
        char next = pattern.charAt(0);
        while (ip < np && im < nm) {
            char c = next;
            ip++;
            next = ip < np ? pattern.charAt(ip) : 0;
            final boolean escaped = c == '\\';
            if (escaped) {
                c = next;
                ip++;
                next = ip < np ? pattern.charAt(ip) : 0;
            }
            if (next == '*') {
                if (!escaped && c == '.') {
                    if (ip >= np - 1) return true;
                    ip++;
                    next = pattern.charAt(ip);
                    if (next == '\\') {
                        ip++;
                        next = ip < np ? pattern.charAt(ip) : 0;
                    }
                    do {
                        if (match.charAt(im) == next) break;
                        im++;
                    } while (im < nm);
                    if (im == nm) return false;
                    ip++;
                    next = ip < np ? pattern.charAt(ip) : 0;
                    im++;
                } else {
                    do {
                        if (match.charAt(im) != c) break;
                        im++;
                    } while (im < nm);
                    ip++;
                    next = ip < np ? pattern.charAt(ip) : 0;
                }
            } else {
                if (c != '.' && match.charAt(im) != c) return false;
                im++;
            }
        }
        if (ip >= np && im >= nm) return true;
        return ip == np - 2 && pattern.charAt(ip) == '.' && pattern.charAt(ip + 1) == '*';
    }

    /* PatternMatcher's advanced glob ('.', [sets] with ranges and '^', '*', '+', {m,n}, "\"), as a regular expression. */
    static boolean advanced(String pattern, String match) {
        StringBuilder re = new StringBuilder();
        for (int k = 0; k < pattern.length(); k++) {
            char c = pattern.charAt(k);
            if (c == '\\' && k + 1 < pattern.length()) {
                re.append(java.util.regex.Pattern.quote(String.valueOf(pattern.charAt(++k))));
            } else if (c == '[') {
                int end = pattern.indexOf(']', k + 1);
                if (end < 0) return false;
                re.append('[');
                for (int j = k + 1; j < end; j++) {
                    char x = pattern.charAt(j);
                    if (x == '\\' && j + 1 < end) re.append('\\').append(pattern.charAt(++j));
                    else if (x == '^' && j == k + 1) re.append('^');
                    else if (x == '-') re.append('-');
                    else if (Character.isLetterOrDigit(x)) re.append(x);
                    else re.append('\\').append(x);
                }
                re.append(']');
                k = end;
            } else if (c == '{') {
                int end = pattern.indexOf('}', k + 1);
                if (end < 0) return false;
                re.append(pattern, k, end + 1);
                k = end;
            } else if (c == '.' || c == '*' || c == '+') {
                re.append(c);
            } else {
                re.append(java.util.regex.Pattern.quote(String.valueOf(c)));
            }
        }
        try {
            return java.util.regex.Pattern.compile(re.toString(), java.util.regex.Pattern.DOTALL).matcher(match).matches();
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** IntentFilter.findMimeType: "image/*" takes every image, "*" and "*\/*" every type; an intent of "*\/*" any typed filter. */
    static boolean findMimeType(List<String> types, String type) {
        if (type == null) return false;
        boolean partial = false;
        for (String v : types) {
            String t = Manifest.val(v);
            if (t == null) continue;
            if (t.equals(type)) return true;
            if (t.equals("*") || t.equals("*/*")) partial = true;
        }
        if (type.equals("*/*")) return !types.isEmpty();
        if (partial) return true;
        int slash = type.indexOf('/');
        if (slash > 0) {
            String base = type.substring(0, slash);
            for (String v : types) {
                String t = Manifest.val(v);
                if (t != null && (t.equals(base) || t.equals(base + "/*"))) return true;
            }
            if (type.length() == slash + 2 && type.charAt(slash + 1) == '*')
                for (String v : types) {
                    String t = Manifest.val(v);
                    if (t != null && t.regionMatches(0, type, 0, slash + 1)) return true;
                }
        }
        return false;
    }

    private static boolean has(List<String> list, String x) {
        for (String v : list) {
            String s = Manifest.val(v);
            if (s != null && s.equals(x)) return true;
        }
        return false;
    }

    /** Intent.isWebIntent: ACTION_VIEW of http or https data. */
    public static boolean isWebIntent(Intent i) {
        Uri d = i.getData();
        String s = d != null ? d.getScheme() : null;
        return Intent.ACTION_VIEW.equals(i.getAction()) && ("http".equals(s) || "https".equals(s));
    }

    /**
     * DomainVerificationUtils.isDomainVerificationIntent: a web link with a host that is resolved for an app to open, whose
     * categories are BROWSABLE and DEFAULT or fewer -- the links Android 12 opens in an app only when the app is approved
     * for the domain.
     */
    public static boolean isDomainVerificationIntent(Intent i, boolean defaultOnly) {
        if (!isWebIntent(i)) return false;
        String host = i.getData().getHost();
        if (host == null || host.isEmpty()) return false;
        Set<String> c = i.getCategories();
        int n = c == null ? 0 : c.size();
        if (n > 2) return false;
        if (n == 2) return c.contains(Intent.CATEGORY_DEFAULT) && c.contains(Intent.CATEGORY_BROWSABLE);
        if (n == 0) return defaultOnly;
        if (c.contains(Intent.CATEGORY_BROWSABLE)) return defaultOnly;
        return c.contains(Intent.CATEGORY_DEFAULT);
    }

    /** IntentFilter.handlesWebUris: VIEW, BROWSABLE, and http or https among its schemes (only those, with `onlyWebSchemes`). */
    public static boolean handlesWebUris(Manifest.Filter f, boolean onlyWebSchemes) {
        if (!f.actions.contains(Intent.ACTION_VIEW) || !has(f.categories, Intent.CATEGORY_BROWSABLE) || f.schemes.isEmpty()) return false;
        for (String v : f.schemes) {
            String s = Manifest.val(v);
            boolean web = "http".equals(s) || "https".equals(s);
            if (onlyWebSchemes ? !web : web) return !onlyWebSchemes;
        }
        return onlyWebSchemes;
    }

    /** IntentFilter.handleAllWebDataURI: a browser's filter -- CATEGORY_APP_BROWSER, or web links of any host. */
    public static boolean handlesAllWebData(Manifest.Filter f) {
        return has(f.categories, Intent.CATEGORY_APP_BROWSER) || (handlesWebUris(f, false) && f.hosts.isEmpty());
    }

    /** Whether the filter declares `host` among its hosts, a wildcard one ("*.example.com") taking its subdomains. */
    public static boolean declaresHost(Manifest.Filter f, String host) {
        if (host == null) return false;
        for (String v : f.hosts) {
            String h = Manifest.val(v);
            if (h == null) continue;
            if (h.startsWith("*") ? host.toLowerCase(java.util.Locale.ROOT).endsWith(h.substring(1).toLowerCase(java.util.Locale.ROOT)) : h.equalsIgnoreCase(host))
                return true;
        }
        return false;
    }
}
