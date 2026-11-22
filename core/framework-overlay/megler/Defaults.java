// Megler: the user's default apps, which every app of Megler's goes by (megler.Resolver): defaults.ini of the apps folder all
// of Megler's packages share (Native.appsFile), a "key=value" line for each --
//   browser=<package>/<activity>         the browser web links open in; none, or "windows": Windows' own browser
//   links.<package>=all | off            which web links that app opens, as Android's "Open supported links": "all", every
//                                        one its filters name; "off", none; none said: those verified for it (AssetLinks)
//   always.<kind>=<package>/<activity>   the answer "Always" of "Open with" for that kind of intent (Resolver.kind), or
//                                        "windows"
// Megler's Settings show and change them too (app/Megler/ui/SettingsScreen.cpp, DefaultApps), and Android's screens of
// them that an app opens (megler.SystemScreens). Read again when it is a second old, as another app or Megler may have
// changed it meanwhile.
package megler;

public final class Defaults {
    private Defaults() {}

    static final String FILE = "defaults.ini", BROWSER = "browser", LINKS = "links.", ALWAYS = "always.";
    /** Windows' own: its browser, or its handler of the link (Native.openUrl). */
    static final String WINDOWS = "windows";

    private static java.util.LinkedHashMap<String, String> sMap = new java.util.LinkedHashMap<>();
    private static long sReadAt = -1;

    /** The value of `key`, or null. */
    public static synchronized String get(String key) {
        load(false);
        return sMap.get(key);
    }

    /** Every key and value, as the file has them now. */
    public static synchronized java.util.Map<String, String> all() {
        load(false);
        return new java.util.LinkedHashMap<>(sMap);
    }

    /** `key` set to `value` (null takes it out), the file read again first so that what another app wrote stays. */
    public static synchronized boolean set(String key, String value) {
        load(true);
        if (value == null ? !sMap.containsKey(key) : value.equals(sMap.get(key))) return true;
        if (value == null) sMap.remove(key);
        else sMap.put(key, value);
        return write();
    }

    /** Every key that begins with `prefix` and whose value is `value` (or any, with null) taken out; how many went. */
    public static synchronized int remove(String prefix, String value) {
        load(true);
        int n = 0;
        for (java.util.Iterator<java.util.Map.Entry<String, String>> it = sMap.entrySet().iterator(); it.hasNext(); ) {
            java.util.Map.Entry<String, String> e = it.next();
            if (e.getKey().startsWith(prefix) && (value == null || value.equals(e.getValue()) || e.getValue().startsWith(value + "/"))) {
                it.remove();
                n++;
            }
        }
        if (n > 0) write();
        return n;
    }

    private static void load(boolean now) {
        long t = android.os.SystemClock.uptimeMillis();
        if (!now && sReadAt >= 0 && t - sReadAt < 1000) return;
        sReadAt = t;
        java.util.LinkedHashMap<String, String> map = new java.util.LinkedHashMap<>();
        byte[] bytes = Native.appsFile(FILE);
        if (bytes != null) {
            for (String line : new String(bytes, java.nio.charset.StandardCharsets.UTF_8).split("\n")) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq <= 0 || line.startsWith("#") || line.startsWith(";")) continue;
                map.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        }
        sMap = map;
    }

    private static boolean write() {
        StringBuilder b = new StringBuilder();
        for (java.util.Map.Entry<String, String> e : sMap.entrySet()) b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        boolean ok = Native.writeAppsFile(FILE, b.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        sReadAt = android.os.SystemClock.uptimeMillis();
        if (!ok) android.util.Log.w("Megler", "default apps: " + FILE + " could not be written");
        return ok;
    }
}
