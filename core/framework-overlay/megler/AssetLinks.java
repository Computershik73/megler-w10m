// Megler: whether a domain's web links are an app's, as Android 12's domain verification has it for each host of an app's
// filters with android:autoVerify: https://<domain>/.well-known/assetlinks.json, read without redirects, names the app's
// package and the SHA-256 of a certificate its APK is signed with, for the relation
// delegate_permission/common.handle_all_urls. ("*.example.com" is verified at example.com.) A link to such a domain opens in
// the app (megler.Resolver), as it does on Android; one to a domain not verified opens in the browser.
//
// Each answer is kept for every app in links-verified.ini of the apps folder (Native.appsFile), "<package> <domain>=yes <time>"
// or "no <time>": a yes for a week, a no for a day; a domain that did not answer (no network, a timeout) is asked again the
// next time.
package megler;

final class AssetLinks {
    private AssetLinks() {}

    static final int UNKNOWN = 0, YES = 1, NO = 2;
    private static final String FILE = "links-verified.ini";
    private static final long WEEK = 7L * 24 * 3600 * 1000, DAY = 24L * 3600 * 1000;

    /** What was found for the app at the domain, as long as it holds: YES, NO, or UNKNOWN when it is to be asked. */
    static synchronized int cached(String pkg, String domain) {
        String v = read().get(pkg + " " + domain.toLowerCase(java.util.Locale.ROOT));
        if (v == null) return UNKNOWN;
        String[] f = v.split(" ");
        long at;
        try { at = Long.parseLong(f.length > 1 ? f[1] : "0"); } catch (NumberFormatException e) { return UNKNOWN; }
        boolean yes = "yes".equals(f[0]);
        long age = System.currentTimeMillis() - at;
        if (age < 0 || age > (yes ? WEEK : DAY)) return UNKNOWN;
        return yes ? YES : NO;
    }

    /**
     * The domain asked whether its links are the app's, whose APK is signed with `certificates` (each a certificate's DER),
     * and the answer kept: YES or NO, or UNKNOWN when it did not answer. Network: never on the main thread.
     */
    static int verify(String pkg, String domain, java.util.List<byte[]> certificates) {
        domain = domain.toLowerCase(java.util.Locale.ROOT);
        if (certificates == null || certificates.isEmpty()) {
            android.util.Log.i("Megler", "links: " + pkg + " cannot be verified for " + domain + ": its certificates are not known");
            return NO;
        }
        java.util.HashSet<String> mine = new java.util.HashSet<>();
        for (byte[] c : certificates) mine.add(fingerprint(c));
        long started = android.os.SystemClock.uptimeMillis();
        String body;
        try {
            java.net.HttpURLConnection h = (java.net.HttpURLConnection) new java.net.URL("https://" + domain + "/.well-known/assetlinks.json").openConnection();
            h.setInstanceFollowRedirects(false);
            h.setConnectTimeout(4000);
            h.setReadTimeout(4000);
            int code = h.getResponseCode();
            if (code != 200) {
                h.disconnect();
                android.util.Log.i("Megler", "links: " + domain + " answers " + code + " for its assetlinks.json: " + pkg + " is not verified for it");
                return keep(pkg, domain, false);
            }
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            try (java.io.InputStream in = h.getInputStream()) {
                byte[] part = new byte[8192];
                for (int n; (n = in.read(part)) > 0; ) {
                    out.write(part, 0, n);
                    if (out.size() > 1 << 20) break;
                }
            }
            h.disconnect();
            body = new String(out.toByteArray(), java.nio.charset.StandardCharsets.UTF_8);
        } catch (Exception e) {
            android.util.Log.i("Megler", "links: " + domain + " did not answer for its assetlinks.json (" + e + "); asked again next time");
            return UNKNOWN;
        }
        boolean yes = false;
        try {
            org.json.JSONArray statements = new org.json.JSONArray(body);
            for (int k = 0; k < statements.length() && !yes; k++) {
                org.json.JSONObject s = statements.optJSONObject(k);
                if (s == null) continue;
                org.json.JSONArray rel = s.optJSONArray("relation");
                boolean all = false;
                for (int r = 0; rel != null && r < rel.length(); r++) if ("delegate_permission/common.handle_all_urls".equals(rel.optString(r))) all = true;
                org.json.JSONObject t = s.optJSONObject("target");
                if (!all || t == null || !"android_app".equals(t.optString("namespace")) || !pkg.equals(t.optString("package_name"))) continue;
                org.json.JSONArray prints = t.optJSONArray("sha256_cert_fingerprints");
                for (int p = 0; prints != null && p < prints.length(); p++)
                    if (mine.contains(prints.optString(p).toUpperCase(java.util.Locale.ROOT))) yes = true;
            }
        } catch (org.json.JSONException e) {
            android.util.Log.i("Megler", "links: " + domain + "'s assetlinks.json is not one (" + e.getMessage() + "): " + pkg + " is not verified for it");
            return keep(pkg, domain, false);
        }
        android.util.Log.i("Megler", "links: " + pkg + (yes ? " is" : " is not") + " verified for " + domain + " by its assetlinks.json ("
            + (android.os.SystemClock.uptimeMillis() - started) + " ms)");
        return keep(pkg, domain, yes);
    }

    /** The SHA-256 of a certificate as assetlinks.json writes it: upper-case hex digits, two by two, with colons. */
    static String fingerprint(byte[] der) {
        try {
            byte[] d = java.security.MessageDigest.getInstance("SHA-256").digest(der);
            StringBuilder b = new StringBuilder();
            for (int k = 0; k < d.length; k++) {
                if (k > 0) b.append(':');
                b.append(String.format(java.util.Locale.ROOT, "%02X", d[k] & 0xff));
            }
            return b.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            return "";
        }
    }

    private static synchronized int keep(String pkg, String domain, boolean yes) {
        java.util.LinkedHashMap<String, String> map = read();
        map.put(pkg + " " + domain, (yes ? "yes " : "no ") + System.currentTimeMillis());
        StringBuilder b = new StringBuilder();
        for (java.util.Map.Entry<String, String> e : map.entrySet()) b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        Native.writeAppsFile(FILE, b.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        return yes ? YES : NO;
    }

    private static java.util.LinkedHashMap<String, String> read() {
        java.util.LinkedHashMap<String, String> map = new java.util.LinkedHashMap<>();
        byte[] bytes = Native.appsFile(FILE);
        if (bytes == null) return map;
        for (String line : new String(bytes, java.nio.charset.StandardCharsets.UTF_8).split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) map.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        return map;
    }
}
