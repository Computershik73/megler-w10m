// Megler: the accounts of android.accounts.AccountManager, which on Android are the system's (AccountManagerService and its
// database), and the authenticators that own their types.
//
// The accounts are kept in one file, /data/megler-services/accounts/accounts.json: the services' folder every app shares
// (core/src/run/mg_run.c), so that an account microG's sign-in adds -- in microG's own app, or in an app that hosts microG --
// is every app's, as on Android. Where there is no such folder, the app's own. Each account has its name, type, password,
// user data and auth tokens. The file is written whole, beside it first and then renamed over it, and read again whenever
// another process has written it since (its time and length), so that an app sees what microG's app added while it ran.
//
// The authenticators are the services that say they are one (the action android.accounts.AccountAuthenticator, with the
// meta-data of that name naming their account-authenticator XML): microG's (megler.MicroG: com.google), and the app's own.
package megler;

import android.accounts.Account;
import android.accounts.AuthenticatorDescription;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

public final class Accounts {
    private Accounts() {}
    public static final String AUTHENTICATOR_ACTION = "android.accounts.AccountAuthenticator";
    private static final String NS = "http://schemas.android.com/apk/res/android";

    /** One account as it is kept. */
    private static final class Entry {
        String name, type, password, previousName;
        final LinkedHashMap<String, String> data = new LinkedHashMap<>(), tokens = new LinkedHashMap<>();
        boolean is(Account a) { return a != null && name.equals(a.name) && type.equals(a.type); }
    }

    private static final ArrayList<Entry> sEntries = new ArrayList<>();
    private static long sTime = -1, sLength = -1;
    private static final ArrayList<Runnable> sListeners = new ArrayList<>();

    /** The file the accounts are kept in. */
    public static File file() {
        File shared = new File(MicroG.SHARED);
        File dir = shared.isDirectory() ? new File(shared, "accounts") : new File(Native.dataDir(), "accounts");
        return new File(dir, "accounts.json");
    }

    /** The accounts as the file has them now: read again when another process wrote it since. True when they changed. */
    private static boolean refresh() {
        File f = file();
        long time = f.lastModified(), length = f.length();
        if (time == sTime && length == sLength) return false;
        sTime = time;
        sLength = length;
        ArrayList<Entry> read = new ArrayList<>();
        if (f.isFile()) {
            try (FileInputStream in = new FileInputStream(f)) {
                byte[] b = new byte[(int) Math.max(0, length)];
                int n = 0, got;
                while (n < b.length && (got = in.read(b, n, b.length - n)) > 0) n += got;
                JSONArray list = new JSONObject(new String(b, 0, n, "UTF-8")).optJSONArray("accounts");
                for (int i = 0; list != null && i < list.length(); i++) {
                    JSONObject o = list.getJSONObject(i);
                    Entry e = new Entry();
                    e.name = o.getString("name");
                    e.type = o.getString("type");
                    e.password = o.has("password") ? o.getString("password") : null;
                    e.previousName = o.has("previousName") ? o.getString("previousName") : null;
                    map(o.optJSONObject("data"), e.data);
                    map(o.optJSONObject("tokens"), e.tokens);
                    read.add(e);
                }
            } catch (Exception ex) {
                android.util.Log.w("Megler", "accounts: " + f + " cannot be read: " + ex);
                return false;
            }
        }
        sEntries.clear();
        sEntries.addAll(read);
        return true;
    }
    private static void map(JSONObject o, Map<String, String> into) throws org.json.JSONException {
        if (o == null) return;
        for (Iterator<String> k = o.keys(); k.hasNext(); ) {
            String key = k.next();
            into.put(key, o.isNull(key) ? null : o.getString(key));
        }
    }
    /** Writes the accounts out, and says they changed. */
    private static void save() {
        File f = file(), next = new File(f.getPath() + ".new");
        f.getParentFile().mkdirs();
        try {
            JSONArray list = new JSONArray();
            for (Entry e : sEntries) {
                JSONObject o = new JSONObject();
                o.put("name", e.name);
                o.put("type", e.type);
                if (e.password != null) o.put("password", e.password);
                if (e.previousName != null) o.put("previousName", e.previousName);
                o.put("data", new JSONObject(e.data));
                o.put("tokens", new JSONObject(e.tokens));
                list.put(o);
            }
            try (FileOutputStream out = new FileOutputStream(next)) {
                out.write(new JSONObject().put("accounts", list).toString().getBytes("UTF-8"));
                out.getFD().sync();
            }
            if (!next.renameTo(f)) throw new java.io.IOException("cannot rename " + next + " to " + f);
            sTime = f.lastModified();
            sLength = f.length();
        } catch (Exception ex) {
            android.util.Log.e("Megler", "accounts: " + f + " cannot be written", ex);
            next.delete();
        }
        changed();
    }
    private static Entry find(Account a) {
        refresh();
        for (Entry e : sEntries) if (e.is(a)) return e;
        return null;
    }

    // ---- the accounts

    public static synchronized Account[] accounts(String type) {
        refresh();
        ArrayList<Account> out = new ArrayList<>();
        for (Entry e : sEntries) if (type == null || type.equals(e.type)) out.add(new Account(e.name, e.type));
        return out.toArray(new Account[0]);
    }
    public static synchronized boolean exists(Account a) { return find(a) != null; }
    public static synchronized boolean add(Account a, String password, Bundle userData) {
        if (a == null || find(a) != null) return false;
        Entry e = new Entry();
        e.name = a.name;
        e.type = a.type;
        e.password = password;
        if (userData != null) for (String k : userData.keySet()) e.data.put(k, userData.getString(k));
        sEntries.add(e);
        save();
        android.util.Log.i("Megler", "accounts: added " + a.type + " account");
        return true;
    }
    public static synchronized boolean remove(Account a) {
        Entry e = find(a);
        if (e == null) return false;
        sEntries.remove(e);
        save();
        android.util.Log.i("Megler", "accounts: removed " + a.type + " account");
        return true;
    }
    public static synchronized Account rename(Account a, String newName) {
        Entry e = find(a);
        if (e == null || newName == null || find(new Account(newName, a.type)) != null) return null;
        e.previousName = e.name;
        e.name = newName;
        e.tokens.clear();
        save();
        return new Account(newName, a.type);
    }
    public static synchronized String previousName(Account a) { Entry e = find(a); return e == null ? null : e.previousName; }
    public static synchronized String password(Account a) { Entry e = find(a); return e == null ? null : e.password; }
    public static synchronized void setPassword(Account a, String password) {
        Entry e = find(a);
        if (e == null) return;
        e.password = password;
        e.tokens.clear();
        save();
    }
    public static synchronized String userData(Account a, String key) { Entry e = find(a); return e == null ? null : e.data.get(key); }
    public static synchronized void setUserData(Account a, String key, String value) {
        Entry e = find(a);
        if (e == null || key == null) return;
        if (value == null) e.data.remove(key); else e.data.put(key, value);
        save();
    }
    public static synchronized String token(Account a, String type) { Entry e = find(a); return e == null ? null : e.tokens.get(type); }
    public static synchronized void setToken(Account a, String type, String token) {
        Entry e = find(a);
        if (e == null || type == null) return;
        if (token == null) e.tokens.remove(type); else e.tokens.put(type, token);
        save();
    }
    /** Forgets a token wherever accounts of the type keep it. */
    public static synchronized void invalidateToken(String accountType, String token) {
        if (token == null) return;
        refresh();
        boolean any = false;
        for (Entry e : sEntries) {
            if (accountType != null && !accountType.equals(e.type)) continue;
            any |= e.tokens.values().removeIf(token::equals);
        }
        if (any) save();
    }

    // ---- who hears that they changed

    /** A listener (AccountManager's), told on the main thread when the accounts changed, here or in another process. */
    public static void listen(Runnable listener, boolean on) {
        synchronized (sListeners) {
            if (on && !sListeners.contains(listener)) sListeners.add(listener);
            if (!on) sListeners.remove(listener);
            if (!sListeners.isEmpty() && !sWatching) { sWatching = true; WATCH.postDelayed(WATCHER, 2000); }
        }
    }
    private static boolean sWatching;
    private static final android.os.Handler WATCH = new android.os.Handler(android.os.Looper.getMainLooper());
    /** While someone listens, the file is looked at every two seconds: another process (microG's sign-in) may add an account. */
    private static final Runnable WATCHER = new Runnable() {
        public void run() {
            boolean changed;
            synchronized (Accounts.class) { changed = refresh(); }
            if (changed) changed();
            synchronized (sListeners) {
                sWatching = !sListeners.isEmpty();
                if (sWatching) WATCH.postDelayed(this, 2000);
            }
        }
    };
    private static void changed() {
        ArrayList<Runnable> all;
        synchronized (sListeners) { all = new ArrayList<>(sListeners); }
        for (Runnable r : all) WATCH.post(r);
        Context app = AppRunner.application() != null ? AppRunner.application() : ContextImpl.app();
        if (app != null) WATCH.post(() -> {
            Broadcasts.send(app, new Intent(android.accounts.AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION));
            Broadcasts.send(app, new Intent(android.accounts.AccountManager.ACTION_VISIBLE_ACCOUNTS_CHANGED));
        });
    }

    // ---- the authenticators

    /** An authenticator: what it is, the intent its service is bound with, and whether it is microG's. */
    public static final class Authenticator {
        public final AuthenticatorDescription description;
        public final Intent intent;
        public final boolean microG;
        Authenticator(AuthenticatorDescription d, Intent i, boolean m) { description = d; intent = i; microG = m; }
    }

    public static synchronized ArrayList<Authenticator> authenticators() {
        ArrayList<Authenticator> out = new ArrayList<>();
        Context gms = MicroG.context(GooglePackages.GMS);
        for (ServiceInfo s : MicroG.services(AUTHENTICATOR_ACTION)) {
            Authenticator a = describe(gms, s.packageName, s.name, s.metaData, true);
            if (a != null) out.add(a);
        }
        Context app = AppRunner.application() != null ? AppRunner.application() : ContextImpl.app();
        for (Manifest.Component c : Manifest.services) {
            if (!c.enabled) continue;
            boolean listed = false;
            for (Manifest.Filter f : c.filters) listed |= f.actions.contains(AUTHENTICATOR_ACTION);
            if (!listed) continue;
            Authenticator a = describe(app, Native.packageName(), c.name, c.metaData, false);
            if (a != null) out.add(a);
        }
        return out;
    }
    public static Authenticator authenticator(String type) {
        if (type == null) return null;
        for (Authenticator a : authenticators()) if (type.equals(a.description.type)) return a;
        return null;
    }
    /** An authenticator's description out of its account-authenticator XML, read with its package's resources. */
    private static Authenticator describe(Context owner, String pkg, String service, Bundle metaData, boolean microG) {
        int xml = metaData == null ? 0 : metaData.getInt(AUTHENTICATOR_ACTION, 0);
        if (owner == null || xml == 0) return null;
        try (XmlResourceParser p = owner.getResources().getXml(xml)) {
            int event;
            while ((event = p.next()) != XmlResourceParser.END_DOCUMENT) {
                if (event != XmlResourceParser.START_TAG || !"account-authenticator".equals(p.getName())) continue;
                String type = p.getAttributeValue(NS, "accountType");
                if (type == null) return null;
                AuthenticatorDescription d = new AuthenticatorDescription(type, pkg, p.getAttributeResourceValue(NS, "label", 0),
                    p.getAttributeResourceValue(NS, "icon", 0), p.getAttributeResourceValue(NS, "smallIcon", 0),
                    p.getAttributeResourceValue(NS, "accountPreferences", 0), p.getAttributeBooleanValue(NS, "customTokens", false));
                return new Authenticator(d, new Intent(AUTHENTICATOR_ACTION).setClassName(pkg, service), microG);
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "accounts: the authenticator " + service + " cannot be read: " + e);
        }
        return null;
    }
}
