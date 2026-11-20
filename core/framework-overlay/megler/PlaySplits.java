// Megler: Play Feature Delivery -- an app's requests for its on-demand feature modules, passed to the Play Store the phone has
// (docs/PLAY-FEATURE-DELIVERY.md). TikTok Lite from Google Play is a small base whose real app is in such modules: its Play
// Core library binds the Play Store's split install service at the first start and asks for them, and with the bind refused
// (another package's service) every request failed and the app never left its first screen.
//
// On Android the app and the Play Store are two processes and Binder is between them. Here they are two processes too, of
// which Windows 10 Mobile runs the one in front, and what they share is a folder (/data/megler-play-splits, which the core
// mounts for every app: mg_run_files). So the service the app binds is a stand-in in its own process, which speaks
// ISplitInstallService as the Play Store's does:
//
//  - the app's side (the stand-in). startInstall is written as a request into <the folder>/<app's package>/<n>.request, and the
//    shell is asked to start the Play Store with it (Native.launchApp of com.android.vending and MARKER + the app's package,
//    which AppRunner.startRequested hands back to requested() there). The stand-in then reads <n>.events as it grows and gives
//    the app each line as the Play Store's service would have: a call of its ISplitInstallServiceCallback, or the session
//    state broadcast Play Core listens for. With no Play Store among the phone's apps the app hears PLAY_STORE_NOT_FOUND at
//    once; with one that takes no request, or says nothing more, SERVICE_DIED -- after so much of the app's own running time,
//    for the app is not run while the Play Store is in front. A request asked again (the app started anew: the phone may end
//    it while the Play Store works) is answered from the events kept, as the Play Store tells an app again of a session it
//    asks for twice -- while what they say is still so: the app of the same version, with the modules installed among its
//    splits;
//  - the Play Store's side. In the Play Store's own process its real service is bound and given the request as Play Core's
//    proxy writes it, as a call of the app that asked: for that one call Binder.getCallingUid() is the app's uid, and while
//    the request is open the app is among the running processes, in front -- what Android would say of the app that asked
//    (it did ask from the front; it is behind only because the phone shows one app). The callback is a recorder, and the
//    session state broadcasts the Play Store sends to the app's package are taken at Broadcasts.send; each becomes a line
//    of <n>.events. Split files the Play Store offers by its FileProvider (SplitCompat's way: status DOWNLOADED with
//    split_file_intents) are copied to files/<session>/<split>.apk beside the events, and the app is given file: URIs of
//    them. Once a line ends the request -- an error, FAILED, CANCELED, INSTALLED, or the files to take -- the shell is asked
//    to show the app again.
//
// Nothing the Play Store verifies with Google is touched: it sees the app's package, uid, version and certificates as it
// sees them of every app the phone has (megler.PhoneApps), and its own account, licence and downloads are its own.
package megler;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.TreeSet;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public final class PlaySplits {
    private PlaySplits() {}
    private static final String TAG = "MeglerPlaySplits";

    static final String ACTION = "com.google.android.play.core.splitinstall.BIND_SPLIT_INSTALL_SERVICE";
    private static final String SERVICE = "com.google.android.play.core.splitinstall.protocol.ISplitInstallService";
    private static final String CALLBACK = "com.google.android.play.core.splitinstall.protocol.ISplitInstallServiceCallback";
    private static final String UPDATE = "com.google.android.play.core.splitinstall.receiver.SplitInstallUpdateIntentService";
    /** What the shell is asked to start in the Play Store for an app's requests: this and the app's package. */
    public static final String MARKER = "megler.PlaySplits.";

    // ISplitInstallService's transactions, and its callback's.
    private static final int START_INSTALL = 2, CANCEL_INSTALL = 4, GET_SESSION_STATE = 5, GET_SESSION_STATES = 6, DEFERRED_UNINSTALL = 7,
                             DEFERRED_INSTALL = 8, DEFERRED_LANGUAGE_INSTALL = 13, DEFERRED_LANGUAGE_UNINSTALL = 14;
    private static final int ON_START_INSTALL = 2, ON_GET_SESSION = 5, ON_ERROR = 6, ON_GET_SESSION_STATES = 7, ON_LAST = 13;
    // SplitInstallErrorCode and SplitInstallSessionStatus.
    private static final int SESSION_NOT_FOUND = -4, API_NOT_AVAILABLE = -5, SERVICE_DIED = -9, PLAY_STORE_NOT_FOUND = -14, INTERNAL_ERROR = -100;
    private static final int DOWNLOADED = 3, INSTALLED = 5, FAILED = 6, CANCELED = 7, REQUIRES_USER_CONFIRMATION = 8;

    /** How often the stand-in looks for new events, and how many looks with nothing new end a request: before the Play Store
     *  took it (20 s) and after (two minutes). The looks are the app's own running time. */
    private static final int TICK_MS = 250, TAKEN_TICKS = 80, QUIET_TICKS = 480;
    /** How long the Play Store's side waits for the first word of the Play Store's service, which says nothing at all to a
     *  call it refuses. */
    private static final int SILENT_MS = 40000;

    private static Handler sMain;
    private static Handler main() {
        if (sMain == null) sMain = new Handler(Looper.getMainLooper());
        return sMain;
    }

    // ---- the files both sides share

    /** A package's name as a folder's: nothing but what a package name has. */
    private static boolean named(String s) {
        if (s == null || s.isEmpty() || s.length() > 200 || s.charAt(0) == '.' || s.contains("..")) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '.')) return false;
        }
        return true;
    }

    /** The folder every app and the Play Store have in common for this (mg_run.c, PLAY_SPLITS_GUEST). */
    private static final String SHARED = "/data/megler-play-splits";

    /** The folder of `app`'s requests, or null where the shared folder is not there. */
    private static File folder(String app) {
        File shared = new File(SHARED);
        return shared.isDirectory() && named(app) ? new File(shared, app) : null;
    }

    /** The number of the app's latest request, 0 with none. */
    private static int latest(File dir) {
        int n = 0;
        String[] names = dir.list();
        if (names != null) for (String name : names) {
            if (!name.endsWith(".request")) continue;
            try { n = Math.max(n, Integer.parseInt(name.substring(0, name.length() - 8))); } catch (NumberFormatException e) { }
        }
        return n;
    }

    private static String read(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (java.io.IOException e) {
            return null;
        }
    }

    /** A file written whole or not at all: the other process never reads half of it. */
    private static boolean write(File f, String text) {
        File tmp = new File(f.getPath() + "." + android.os.Process.myPid() + ".new");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (java.io.IOException e) {
            tmp.delete();
            return false;
        }
        f.delete();
        return tmp.renameTo(f);
    }

    /** One more line of a request's events. */
    private static synchronized boolean append(File events, JSONObject line) {
        try (FileOutputStream out = new FileOutputStream(events, true)) {
            out.write((line.toString() + "\n").getBytes(StandardCharsets.UTF_8));
            return true;
        } catch (java.io.IOException e) {
            Log.w(TAG, "an event could not be written: " + e);
            return false;
        }
    }

    /** A request's events so far, whole lines only (the last may be half written). */
    private static ArrayList<JSONObject> events(File events) {
        ArrayList<JSONObject> all = new ArrayList<>();
        String text = events.isFile() ? read(events) : null;
        if (text == null) return all;
        int from = 0;
        for (int nl; (nl = text.indexOf('\n', from)) >= 0; from = nl + 1) {
            try { all.add(new JSONObject(text.substring(from, nl))); } catch (JSONException e) { Log.w(TAG, "an event that is not one: " + e); }
        }
        return all;
    }

    /** What ends a request: an error, a session FAILED, CANCELED or INSTALLED, one that waits for the user, or files to take. */
    private static boolean ends(JSONObject e) {
        if (e.has("lost")) return true;
        if (e.optInt("callback") == ON_ERROR) return true;
        JSONObject state = e.optJSONObject("state");
        if (state == null) return false;
        int status = number(state, "status");
        return status == INSTALLED || status == FAILED || status == CANCELED || status == REQUIRES_USER_CONFIRMATION || (status == DOWNLOADED && state.has("split_file_intents"));
    }

    /** Whether a request ended with the modules the app's: INSTALLED, or their files there to take. */
    private static boolean wentWell(JSONObject e) {
        JSONObject state = e.optJSONObject("state");
        if (state == null) return false;
        int status = number(state, "status");
        if (status == INSTALLED) return true;
        if (status != DOWNLOADED || !state.has("split_file_intents")) return false;
        try {
            JSONArray intents = state.getJSONArray("split_file_intents").getJSONArray(1);
            for (int i = 0; i < intents.length(); i++) {
                Uri data = Uri.parse(intents.getJSONArray(i).getJSONObject(1).optString("data"));
                if (!"file".equals(data.getScheme()) || !new File(data.getPath()).isFile()) return false;
            }
            return true;
        } catch (JSONException x) {
            return false;
        }
    }

    /**
     * Why what a request that went well gave the app is the app's no longer, or null while it still is. The app is of another
     * version now: Google Play updated it, and with an update it installs the modules it knows the app has, and SplitCompat's
     * files of the version before are not this one's. Or a module the Play Store installed into the app (INSTALLED) is not
     * among the splits it runs with: it was installed again without it. TikTok Lite from Google Play was told INSTALLED of
     * df_edit_filter so at every start after Google Play had updated it without that module, and waited for it for ever
     * (issue #6, 0.1.0.734).
     */
    private static String gone(JSONObject request, JSONObject e) {
        Manifest.read();
        long then = request.optLong("version", -1);
        if (then >= 0 && then != Manifest.versionCode) return "the app was of version " + then + " then, and is of " + Manifest.versionCode + " now";
        JSONObject state = e.optJSONObject("state");
        if (state == null || number(state, "status") != INSTALLED) return null;
        String[] have = android.content.pm.ApplicationInfo.self().splitNames;
        java.util.List<String> splits = have != null ? java.util.Arrays.asList(have) : new ArrayList<String>();
        JSONArray list = request.optJSONArray("list");
        if (list != null) for (int i = 0; i < list.length(); i++) {
            JSONObject b = list.optJSONObject(i);
            JSONArray module = b != null ? b.optJSONArray("module_name") : null;
            if (module != null && !splits.contains(module.optString(1))) return "the app has no module " + module.optString(1) + " among its splits";
        }
        return null;
    }

    /** An int of a bundle as json() wrote it, or 0. */
    private static int number(JSONObject bundle, String key) {
        JSONArray v = bundle.optJSONArray(key);
        return v != null ? v.optInt(1) : 0;
    }

    // ---- bundles and intents as text. A value is [type, value]; what has no type here is left out, and said.

    static JSONObject json(Bundle b) throws JSONException {
        JSONObject o = new JSONObject();
        if (b == null) return o;
        for (String key : b.keySet()) {
            Object v = b.get(key);
            JSONArray j = json(v);
            if (j != null) o.put(key, j);
            else if (v != null) Log.w(TAG, "the " + v.getClass().getName() + " under \"" + key + "\" has no way across; it is left out");
        }
        return o;
    }

    private static JSONArray json(Object v) throws JSONException {
        JSONArray a = new JSONArray();
        if (v instanceof Integer) return a.put("int").put(((Integer) v).intValue());
        if (v instanceof Long) return a.put("long").put(((Long) v).longValue());
        if (v instanceof Boolean) return a.put("boolean").put(((Boolean) v).booleanValue());
        if (v instanceof String) return a.put("string").put((String) v);
        if (v instanceof Bundle) return a.put("bundle").put(json((Bundle) v));
        if (v instanceof Intent) {
            Intent i = (Intent) v;
            JSONObject o = new JSONObject();
            if (i.getAction() != null) o.put("action", i.getAction());
            if (i.getData() != null) o.put("data", i.getData().toString());
            if (i.getType() != null) o.put("type", i.getType());
            if (i.getPackage() != null) o.put("package", i.getPackage());
            if (i.getComponent() != null) o.put("component", i.getComponent().flattenToString());
            o.put("flags", i.getFlags());
            if (i.getExtras() != null) o.put("extras", json(i.getExtras()));
            return a.put("intent").put(o);
        }
        JSONArray items = new JSONArray();
        if (v instanceof long[]) {
            for (long x : (long[]) v) items.put(x);
            return a.put("longs").put(items);
        }
        if (v instanceof int[]) {
            for (int x : (int[]) v) items.put(x);
            return a.put("ints").put(items);
        }
        if (v instanceof String[]) {
            for (String x : (String[]) v) items.put(x);
            return a.put("strings").put(items);
        }
        if (v instanceof java.util.List) {
            for (Object x : (java.util.List<?>) v) {
                JSONArray j = json(x);
                if (j == null) return null;
                items.put(j);
            }
            return a.put("list").put(items);
        }
        return null;
    }

    static Bundle bundle(JSONObject o) throws JSONException {
        Bundle b = new Bundle();
        if (o == null) return b;
        for (java.util.Iterator<String> keys = o.keys(); keys.hasNext(); ) {
            String key = keys.next();
            JSONArray v = o.getJSONArray(key);
            switch (v.getString(0)) {
            case "int": b.putInt(key, v.getInt(1)); break;
            case "long": b.putLong(key, v.getLong(1)); break;
            case "boolean": b.putBoolean(key, v.getBoolean(1)); break;
            case "string": b.putString(key, v.getString(1)); break;
            case "bundle": b.putBundle(key, bundle(v.getJSONObject(1))); break;
            case "intent": b.putParcelable(key, intent(v.getJSONObject(1))); break;
            case "longs": {
                JSONArray items = v.getJSONArray(1);
                long[] x = new long[items.length()];
                for (int i = 0; i < x.length; i++) x[i] = items.getLong(i);
                b.putLongArray(key, x);
                break;
            }
            case "ints": {
                JSONArray items = v.getJSONArray(1);
                int[] x = new int[items.length()];
                for (int i = 0; i < x.length; i++) x[i] = items.getInt(i);
                b.putIntArray(key, x);
                break;
            }
            case "strings": {
                JSONArray items = v.getJSONArray(1);
                String[] x = new String[items.length()];
                for (int i = 0; i < x.length; i++) x[i] = items.getString(i);
                b.putStringArray(key, x);
                break;
            }
            case "list": {
                // of strings, of numbers, or of bundles and intents: the three kinds of list a Bundle keeps
                JSONArray items = v.getJSONArray(1);
                String kind = items.length() > 0 ? items.getJSONArray(0).getString(0) : "string";
                if (kind.equals("string")) {
                    ArrayList<String> x = new ArrayList<>();
                    for (int i = 0; i < items.length(); i++) x.add(items.getJSONArray(i).getString(1));
                    b.putStringArrayList(key, x);
                } else if (kind.equals("int")) {
                    ArrayList<Integer> x = new ArrayList<>();
                    for (int i = 0; i < items.length(); i++) x.add(items.getJSONArray(i).getInt(1));
                    b.putIntegerArrayList(key, x);
                } else {
                    ArrayList<android.os.Parcelable> x = new ArrayList<>();
                    for (int i = 0; i < items.length(); i++) {
                        JSONArray item = items.getJSONArray(i);
                        x.add(item.getString(0).equals("intent") ? intent(item.getJSONObject(1)) : bundle(item.getJSONObject(1)));
                    }
                    b.putParcelableArrayList(key, x);
                }
                break;
            }
            default: break;
            }
        }
        return b;
    }

    private static Intent intent(JSONObject o) throws JSONException {
        Intent i = new Intent(o.optString("action", null));
        Uri data = o.has("data") ? Uri.parse(o.getString("data")) : null;
        String type = o.optString("type", null);
        if (data != null && type != null) i.setDataAndType(data, type);
        else if (data != null) i.setData(data);
        else if (type != null) i.setType(type);
        if (o.has("package")) i.setPackage(o.getString("package"));
        if (o.has("component")) i.setComponent(ComponentName.unflattenFromString(o.getString("component")));
        i.setFlags(o.optInt("flags"));
        if (o.has("extras")) i.putExtras(bundle(o.getJSONObject("extras")));
        return i;
    }

    private static JSONArray json(java.util.List<Bundle> list) throws JSONException {
        JSONArray a = new JSONArray();
        if (list != null) for (Bundle b : list) a.put(json(b));
        return a;
    }

    private static ArrayList<Bundle> bundles(JSONArray a) throws JSONException {
        ArrayList<Bundle> list = new ArrayList<>();
        if (a != null) for (int i = 0; i < a.length(); i++) list.add(bundle(a.getJSONObject(i)));
        return list;
    }

    /** What a request asks for, the same for the same modules and languages in any order. */
    private static String asked(java.util.List<Bundle> list) {
        TreeSet<String> names = new TreeSet<>();
        if (list != null) for (Bundle b : list) {
            if (b.getString("module_name") != null) names.add("module " + b.getString("module_name"));
            if (b.getString("language") != null) names.add("language " + b.getString("language"));
        }
        return names.toString();
    }

    // ---- the app's side: the stand-in for the Play Store's service

    /** PlayStore.bind: the binder for an app's bind of the Play Store's split install service; null in the Play Store itself,
     *  whose own service that is. */
    static IBinder bind(Intent i) {
        if (!ACTION.equals(i.getAction()) || GooglePackages.VENDING.equals(Native.packageName())) return null;
        Log.i(TAG, "the app binds the Play Store's split install service: its requests for feature modules go to the Play Store the phone has");
        return STAND_IN;
    }

    private static final IBinder STAND_IN = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code < START_INSTALL || code > DEFERRED_LANGUAGE_UNINSTALL) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(SERVICE);
            final String pkg = data.readString();
            switch (code) {
            case START_INSTALL: case DEFERRED_UNINSTALL: case DEFERRED_INSTALL: case DEFERRED_LANGUAGE_INSTALL: case DEFERRED_LANGUAGE_UNINSTALL: {
                ArrayList<Bundle> list = data.createTypedArrayList(Bundle.CREATOR);
                Bundle options = data.readInt() != 0 ? Bundle.CREATOR.createFromParcel(data) : null;
                IBinder callback = data.readStrongBinder();
                if (code == START_INSTALL) startInstall(pkg, list, options, callback);
                // Installs and uninstalls "when the system sees fit", in the background: the Play Store here works only while it
                // is in front, and showing it for something the app asked to have done out of sight is not that. That part of
                // the API is not here, and its error says so.
                else error(callback, API_NOT_AVAILABLE, "a deferred install or uninstall (transaction " + code + ") asked for " + asked(list));
                return true;
            }
            case CANCEL_INSTALL: {
                int session = data.readInt();
                // Play Core since 2.0 sends its options here too; the Play Store's service of 2025 reads none.
                Bundle options = null;
                boolean withOptions = data.dataAvail() > 8;
                if (withOptions && data.readInt() != 0) options = Bundle.CREATOR.createFromParcel(data);
                cancelInstall(pkg, session, withOptions, options, data.readStrongBinder());
                return true;
            }
            case GET_SESSION_STATE: {
                int session = data.readInt();
                sessionStates(pkg, session, data.readStrongBinder());
                return true;
            }
            case GET_SESSION_STATES:
                sessionStates(pkg, -1, data.readStrongBinder());
                return true;
            default:
                return super.onTransact(code, data, reply, flags);
            }
        }
    };

    /** One call of the app's callback: the session where the call has one (`session` >= 0), then a bundle or a list of them. */
    private static void call(IBinder callback, int code, int session, Bundle bundle, ArrayList<Bundle> list) {
        if (callback == null) return;
        Parcel p = Parcel.obtain();
        try {
            p.writeInterfaceToken(CALLBACK);
            if (code >= ON_START_INSTALL && code <= ON_GET_SESSION) p.writeInt(session);
            if (code == ON_GET_SESSION_STATES) p.writeTypedList(list != null ? list : new ArrayList<Bundle>());
            else if (bundle != null) {
                p.writeInt(1);
                bundle.writeToParcel(p, 0);
            } else p.writeInt(0);
            callback.transact(code, p, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "the app's callback (call " + code + ") threw " + e);
        } finally {
            p.recycle();
        }
    }

    private static void error(IBinder callback, int code, String why) {
        Log.i(TAG, "the app is told error " + code + ": " + why);
        Bundle b = new Bundle();
        b.putInt("error_code", code);
        call(callback, ON_ERROR, -1, b, null);
    }

    /** The requests the app waits for in this process, by their number. */
    private static final HashMap<Integer, Follow> sFollowed = new HashMap<>();

    private static synchronized void startInstall(String pkg, ArrayList<Bundle> list, Bundle options, IBinder callback) {
        String asked = asked(list);
        if (PhoneApps.find(GooglePackages.VENDING) == null) {
            error(callback, PLAY_STORE_NOT_FOUND, "the phone has no Play Store to ask for " + asked);
            return;
        }
        File dir = folder(pkg);
        if (dir == null || !(dir.isDirectory() || dir.mkdirs())) {
            error(callback, SERVICE_DIED, "there is no folder to pass the request for " + asked + " to the Play Store in");
            return;
        }
        int n = latest(dir);
        // The same asked again: answered from what the Play Store said of it, if that went well and is still so; passed again,
        // if it has not ended. One that failed, or whose module the app no longer has, is asked anew.
        boolean again = false, ended = false;
        String gone = null;
        if (n > 0) try {
            JSONObject last = new JSONObject(String.valueOf(read(new File(dir, n + ".request"))));
            if (last.optInt("code") == START_INSTALL && asked.equals(last.optString("asked"))) {
                again = true;
                JSONObject end = null;
                for (JSONObject e : events(new File(dir, n + ".events"))) if (ends(e)) end = e;
                if (end != null) {
                    ended = true;
                    again = wentWell(end) && (gone = gone(last, end)) == null;
                }
            }
        } catch (JSONException e) {
            again = false;
        }
        if (gone != null) Log.i(TAG, "request " + n + " (" + asked + ") asked again: what the Play Store said of it is so no longer (" + gone + "); asked anew");
        Follow f = again ? sFollowed.get(n) : null;
        if (f != null) {
            // twice in one run: the second callback hears what the first did, and what comes
            f.join(callback);
            return;
        }
        if (!again) {
            n++;
            try {
                // (with the app's version, which what comes of it is for: gone)
                Manifest.read();
                JSONObject r = new JSONObject().put("code", START_INSTALL).put("package", pkg).put("asked", asked).put("version", Manifest.versionCode).put("list", json(list));
                if (options != null) r.put("options", json(options));
                new File(dir, n + ".events").delete();
                if (!write(new File(dir, n + ".request"), r.toString())) throw new JSONException("not written");
            } catch (JSONException e) {
                error(callback, SERVICE_DIED, "the request for " + asked + " could not be written for the Play Store: " + e.getMessage());
                return;
            }
        }
        f = new Follow(pkg, new File(dir, n + ".events"), n, again && ended);
        f.callbacks.add(callback);
        if (again && ended) {
            Log.i(TAG, "request " + n + " (" + asked + ") asked again: answered from what the Play Store said of it");
        } else {
            Log.i(TAG, "request " + n + " (" + asked + ")" + (again ? " asked again before it ended" : "") + ": the Play Store is started for it");
            if (!Native.launchApp(GooglePackages.VENDING, MARKER + pkg)) {
                error(callback, SERVICE_DIED, "the Play Store could not be started for request " + n);
                return;
            }
            sFollowed.put(n, f);
        }
        main().post(f);
    }

    /** A request the app waits for: its events, given to the app as they come. */
    private static final class Follow implements Runnable {
        final String pkg;
        final File events;
        final int id;
        final boolean kept;
        final ArrayList<IBinder> callbacks = new ArrayList<>();
        int at, quiet;
        boolean taken, over;
        Follow(String pkg, File events, int id, boolean kept) { this.pkg = pkg; this.events = events; this.id = id; this.kept = kept; }

        /** Another callback for the same request: it hears the calls the first has heard. */
        void join(IBinder callback) {
            ArrayList<JSONObject> all = events(events);
            for (int i = 0; i < at && i < all.size(); i++) if (all.get(i).has("callback")) give(all.get(i), callback, false);
            callbacks.add(callback);
            Log.i(TAG, "request " + id + " asked again while it is open: the second callback hears it too");
        }

        @Override
        public void run() {
            if (over) return;
            ArrayList<JSONObject> all = events(events);
            boolean news = at < all.size();
            for (; at < all.size() && !over; at++) {
                JSONObject e = all.get(at);
                if (e.has("taken")) taken = true;
                for (int i = 0; i < callbacks.size(); i++) give(e, callbacks.get(i), i == 0);
                over = ends(e);
            }
            quiet = news ? 0 : quiet + 1;
            if (!over && (kept || quiet > (taken ? QUIET_TICKS : TAKEN_TICKS))) {
                // (events kept from an earlier run end the request, or they would not be given)
                for (IBinder callback : callbacks)
                    error(callback, SERVICE_DIED, taken ? "the Play Store said nothing more of request " + id : "the Play Store did not take request " + id);
                over = true;
            }
            if (over) {
                synchronized (PlaySplits.class) { sFollowed.remove(id); }
                Log.i(TAG, "request " + id + " is over");
                return;
            }
            main().postDelayed(this, TICK_MS);
        }

        /** One event to one callback: its call, or -- once, for all of them -- the broadcast Play Core's receiver hears. */
        private void give(JSONObject e, IBinder callback, boolean first) {
            try {
                if (e.has("lost")) {
                    error(callback, e.getInt("lost"), e.optString("why"));
                } else if (e.has("callback")) {
                    int code = e.getInt("callback");
                    Log.i(TAG, "request " + id + ": the callback's call " + code + (e.has("session") ? ", session " + e.getInt("session") : "")
                               + (code == ON_ERROR ? ", error " + number(e.getJSONObject("bundle"), "error_code") : ""));
                    call(callback, code, e.optInt("session", -1), e.has("bundle") ? bundle(e.getJSONObject("bundle")) : null, e.has("list") ? bundles(e.getJSONArray("list")) : null);
                } else if (e.has("state") && first) {
                    Bundle state = bundle(e.getJSONObject("state"));
                    Context app = AppRunner.application();
                    Log.i(TAG, "request " + id + ": session " + state.getInt("session_id") + " is in state " + state.getInt("status")
                               + (state.getInt("error_code") != 0 ? ", error " + state.getInt("error_code") : "")
                               + (state.containsKey("split_file_intents") ? ", with " + state.getParcelableArrayList("split_file_intents").size() + " split files to take" : "")
                               + (state.getLong("total_bytes_to_download") > 0 ? ", " + state.getLong("bytes_downloaded") + " of " + state.getLong("total_bytes_to_download") + " bytes" : ""));
                    // as the Play Store sends it: to the app's package alone, to receivers the app registered
                    if (app != null) app.sendBroadcast(new Intent(UPDATE).setPackage(pkg).putExtra("session_state", state).addFlags(0x40000000 /* FLAG_RECEIVER_REGISTERED_ONLY */));
                }
            } catch (JSONException | RuntimeException x) {
                Log.w(TAG, "request " + id + ": an event could not be given to the app: " + x);
            }
        }
    }

    /**
     * cancelInstall: passed to the Play Store when the session is one of an open request of this run; any other is no session
     * the Play Store could still be working on here, and the answer is the one it gives for a session it does not know.
     */
    private static synchronized void cancelInstall(String pkg, int session, boolean withOptions, Bundle options, IBinder callback) {
        File dir = folder(pkg);
        for (Follow f : sFollowed.values()) {
            boolean its = false;
            for (JSONObject e : events(f.events)) its |= e.optInt("callback") == ON_START_INSTALL && e.optInt("session", -1) == session;
            if (!its || dir == null) continue;
            int n = latest(dir) + 1;
            try {
                JSONObject r = new JSONObject().put("code", CANCEL_INSTALL).put("package", pkg).put("session", session).put("withOptions", withOptions);
                if (options != null) r.put("options", json(options));
                if (!write(new File(dir, n + ".request"), r.toString()) || !Native.launchApp(GooglePackages.VENDING, MARKER + pkg)) break;
                Log.i(TAG, "request " + n + " (cancel session " + session + "): the Play Store is started for it");
                Follow c = new Follow(pkg, new File(dir, n + ".events"), n, false);
                c.callbacks.add(callback);
                sFollowed.put(n, c);
                main().post(c);
                return;
            } catch (JSONException e) {
                break;
            }
        }
        error(callback, SESSION_NOT_FOUND, "session " + session + " to cancel is none the Play Store is working on for this app");
    }

    /**
     * getSessionState and getSessionStates, from what the Play Store said of the app's latest request: its session's last
     * state while the request is open, as the sessions of an app are all its requests here; none once it ended, as the Play
     * Store keeps none of an ended session. The Play Store is not shown for a question.
     */
    private static void sessionStates(String pkg, int session, IBinder callback) {
        File dir = folder(pkg);
        Bundle last = null;
        boolean over = false;
        int n = dir != null && dir.isDirectory() ? latest(dir) : 0;
        if (n > 0) for (JSONObject e : events(new File(dir, n + ".events"))) {
            over |= ends(e);
            try {
                if (e.has("state")) last = bundle(e.getJSONObject("state"));
                else if (e.optInt("callback") == ON_START_INSTALL && e.has("bundle")) last = bundle(e.getJSONObject("bundle"));
            } catch (JSONException x) { }
        }
        if (over || (last != null && !last.containsKey("session_id"))) last = null;
        if (session < 0) {
            ArrayList<Bundle> list = new ArrayList<>();
            if (last != null) list.add(last);
            Log.i(TAG, "the app asks for its sessions: " + list.size());
            call(callback, ON_GET_SESSION_STATES, -1, null, list);
        } else if (last != null && last.getInt("session_id") == session) {
            call(callback, ON_GET_SESSION, session, last, null);
        } else {
            error(callback, SESSION_NOT_FOUND, "session " + session + " asked about is none of an open request");
        }
    }

    // ---- the Play Store's side

    /** A request being passed to the Play Store's service. */
    private static final class Served {
        final String app;
        final int id, uid;
        final File dir, events;
        final JSONObject request;
        boolean answered, over;
        Served(String app, int uid, File dir, int id, JSONObject request) {
            this.app = app; this.uid = uid; this.dir = dir; this.id = id; this.request = request;
            events = new File(dir, id + ".events");
        }
    }
    /** The open requests, by the app's package: one of an app at a time, its latest. */
    private static final HashMap<String, Served> sServed = new HashMap<>();
    private static IBinder sService;
    private static boolean sBinding;
    /** What the Play Store's service says is written in the order it was said, off its threads. */
    private static java.util.concurrent.ExecutorService sWriter;

    /** The apps whose requests are open: ActivityManager.getRunningAppProcesses lists them, in front, as they were when they asked. */
    public static synchronized String[] served() {
        return sServed.isEmpty() ? null : sServed.keySet().toArray(new String[0]);
    }

    /**
     * AppRunner.startRequested: whether `cls` is no activity but the word that an app has requests here (MARKER and its
     * package), which are then taken up -- in the Play Store; any other app has nothing to do with it.
     */
    public static boolean requested(String cls) {
        if (cls == null || !cls.startsWith(MARKER)) return false;
        String app = cls.substring(MARKER.length());
        if (!GooglePackages.VENDING.equals(Native.packageName())) {
            Log.w(TAG, "asked to take up " + app + "'s requests for feature modules, which only the Play Store can");
            return true;
        }
        serve(app);
        return true;
    }

    private static synchronized void serve(String app) {
        File dir = folder(app);
        PhoneApps.App asking = named(app) ? PhoneApps.find(app) : null;
        int n = dir != null && dir.isDirectory() ? latest(dir) : 0;
        if (asking == null || n == 0) {
            Log.w(TAG, "asked to take up the requests of " + app + ", " + (asking == null ? "which is no app the phone has" : "which has none"));
            return;
        }
        File events = new File(dir, n + ".events");
        JSONObject request;
        try {
            request = new JSONObject(String.valueOf(read(new File(dir, n + ".request"))));
        } catch (JSONException e) {
            Log.w(TAG, "request " + n + " of " + app + " cannot be read: " + e.getMessage());
            return;
        }
        for (JSONObject e : events(events)) {
            if (!ends(e)) continue;
            Log.i(TAG, "request " + n + " of " + app + " has ended already; nothing to pass");
            return;
        }
        Served old = sServed.get(app);
        if (old != null) old.over = true;
        final Served s = new Served(app, asking.uid, dir, n, request);
        sServed.put(app, s);
        if (sWriter == null) sWriter = java.util.concurrent.Executors.newSingleThreadExecutor();
        // The split files of the app's earlier requests: taken by now, or of a request given up.
        if (request.optInt("code") == START_INSTALL) deleteTree(new File(dir, "files"));
        try { append(events, new JSONObject().put("taken", true)); } catch (JSONException e) { }
        Log.i(TAG, "request " + n + " of " + app + " (" + (request.optInt("code") == START_INSTALL ? request.optString("asked") : "cancel session " + request.optInt("session"))
                   + ") is taken up: passed to the Play Store's split install service as the app's call");
        if (sService != null) {
            pass(s);
            return;
        }
        if (sBinding) return;
        Context context = AppRunner.application();
        sBinding = context != null && context.bindService(new Intent(ACTION).setPackage(GooglePackages.VENDING), new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                ArrayList<Served> open;
                synchronized (PlaySplits.class) {
                    sService = service;
                    sBinding = false;
                    open = new ArrayList<>(sServed.values());
                }
                for (Served o : open) pass(o);
            }
            @Override public void onServiceDisconnected(ComponentName name) {
                synchronized (PlaySplits.class) { sService = null; }
            }
        }, Context.BIND_AUTO_CREATE);
        if (!sBinding) lost(s, SERVICE_DIED, "this Play Store has no split install service to bind");
    }

    /** The request as Play Core's proxy writes it, to the Play Store's service, as a call of the app that asked. */
    private static void pass(final Served s) {
        IBinder service;
        synchronized (PlaySplits.class) {
            service = sService;
            if (s.over || service == null) return;
        }
        Parcel p = Parcel.obtain();
        try {
            int code = s.request.optInt("code");
            Bundle options = s.request.has("options") ? bundle(s.request.getJSONObject("options")) : null;
            p.writeInterfaceToken(SERVICE);
            p.writeString(s.request.optString("package"));
            if (code == START_INSTALL) {
                p.writeTypedList(bundles(s.request.optJSONArray("list")));
                if (options != null) {
                    p.writeInt(1);
                    options.writeToParcel(p, 0);
                } else p.writeInt(0);
            } else {
                p.writeInt(s.request.optInt("session"));
                if (s.request.optBoolean("withOptions")) {
                    if (options != null) {
                        p.writeInt(1);
                        options.writeToParcel(p, 0);
                    } else p.writeInt(0);
                }
            }
            p.writeStrongBinder(new Recorder(s));
            // The service asks whose call this is before anything else, on the calling thread (Binder.getCallingUid, and the
            // packages of that uid): the app's, as Binder would tell it on Android.
            Binder.meglerCallingUid(s.uid);
            try {
                service.transact(code, p, null, IBinder.FLAG_ONEWAY);
            } finally {
                Binder.meglerCallingUid(-1);
            }
        } catch (JSONException | RemoteException | RuntimeException e) {
            lost(s, INTERNAL_ERROR, "the request could not be given to the Play Store's service: " + e);
            return;
        } finally {
            p.recycle();
        }
        // A call the service refuses (a package that is not the caller's, one its flags leave out) it does not answer.
        main().postDelayed(() -> {
            if (!s.answered && !s.over) lost(s, SERVICE_DIED, "the Play Store's service did not answer in " + SILENT_MS / 1000 + " s");
        }, SILENT_MS);
    }

    /** The callback the Play Store's service is given: each call is a line of the request's events. */
    private static final class Recorder extends Binder {
        final Served s;
        Recorder(Served s) { this.s = s; }
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code < ON_START_INSTALL || code > ON_LAST) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(CALLBACK);
            final JSONObject e = new JSONObject();
            try {
                e.put("callback", code);
                if (code <= ON_GET_SESSION) e.put("session", data.readInt());
                if (code == ON_GET_SESSION_STATES) e.put("list", json(data.createTypedArrayList(Bundle.CREATOR)));
                else if (data.readInt() != 0) e.put("bundle", json(Bundle.CREATOR.createFromParcel(data)));
            } catch (JSONException x) {
                Log.w(TAG, "request " + s.id + " of " + s.app + ": the service's call " + code + " could not be kept: " + x);
                return true;
            }
            s.answered = true;
            sWriter.execute(() -> record(s, e));
            return true;
        }
    }

    /**
     * Broadcasts.send: a session state the Play Store sends to the app whose request is open -- kept as a line of its
     * events, for the app's side to send in the app. True when the broadcast was that (it has no receiver in this process).
     */
    static boolean broadcast(Intent i) {
        if (i == null || !UPDATE.equals(i.getAction()) || i.getPackage() == null || !GooglePackages.VENDING.equals(Native.packageName())) return false;
        final Served s;
        synchronized (PlaySplits.class) { s = sServed.get(i.getPackage()); }
        final Bundle state = i.getBundleExtra("session_state");
        if (s == null || state == null) {
            Log.i(TAG, "a session state for " + i.getPackage() + ", which has no open request here: not passed on");
            return true;
        }
        s.answered = true;
        sWriter.execute(() -> {
            try {
                // The split files SplitCompat is to take, which the Play Store names by its FileProvider's URIs and lets the
                // app read: read here, where they can be, into files of the shared folder, and named by those.
                ArrayList<Intent> files = state.getParcelableArrayList("split_file_intents");
                if (files != null && state.getInt("status") == DOWNLOADED) {
                    ArrayList<Intent> copies = new ArrayList<>();
                    File to = new File(new File(s.dir, "files"), String.valueOf(state.getInt("session_id")));
                    to.mkdirs();
                    for (Intent f : files) {
                        String split = f.getStringExtra("split_id");
                        File copy = new File(to, split + ".apk");
                        if (!named(split) || !copy(f.getData(), copy)) {
                            lost(s, INTERNAL_ERROR, "the file of the split " + split + " could not be copied out of the Play Store");
                            return;
                        }
                        copies.add(new Intent(f).setData(Uri.fromFile(copy)));
                    }
                    state.putParcelableArrayList("split_file_intents", copies);
                    Log.i(TAG, "request " + s.id + " of " + s.app + ": " + copies.size() + " split files copied for the app to take");
                }
                // (a PendingIntent of this process is nothing the app's can start)
                state.remove("user_confirmation_intent");
                record(s, new JSONObject().put("state", json(state)));
            } catch (JSONException | RuntimeException x) {
                lost(s, INTERNAL_ERROR, "a session state could not be kept: " + x);
            }
        });
        return true;
    }

    private static boolean copy(Uri from, File to) {
        Context context = AppRunner.application();
        if (from == null || context == null) return false;
        File tmp = new File(to.getPath() + ".new");
        try (InputStream in = context.getContentResolver().openInputStream(from); FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        } catch (java.io.IOException | RuntimeException e) {
            Log.w(TAG, "a split file could not be read from the Play Store: " + e);
            tmp.delete();
            return false;
        }
        to.delete();
        return tmp.renameTo(to);
    }

    /** One more event of a request; the one that ends it brings the app back. */
    private static void record(Served s, JSONObject e) {
        if (s.over) return;
        append(s.events, e);
        JSONObject state = e.optJSONObject("state");
        Log.i(TAG, "request " + s.id + " of " + s.app + ": the Play Store " + (state != null ? "says session " + number(state, "session_id") + " is in state " + number(state, "status")
                   + (number(state, "error_code") != 0 ? ", error " + number(state, "error_code") : "")
                   : e.has("lost") ? "is lost (error " + e.optInt("lost") + "): " + e.optString("why") : "calls the callback (call " + e.optInt("callback") + ")"));
        if (!ends(e)) return;
        synchronized (PlaySplits.class) {
            s.over = true;
            if (sServed.get(s.app) == s) sServed.remove(s.app);
        }
        Log.i(TAG, "request " + s.id + " of " + s.app + " is over: the app is shown again");
        Native.launchApp(s.app, null);
    }

    /** The request ends without the Play Store's word: the app is told `code`. */
    private static void lost(Served s, int code, String why) {
        try { record(s, new JSONObject().put("lost", code).put("why", why)); } catch (JSONException e) { }
    }

    private static void deleteTree(File f) {
        File[] inside = f.listFiles();
        if (inside != null) for (File x : inside) deleteTree(x);
        f.delete();
    }
}
