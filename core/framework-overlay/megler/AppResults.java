// Megler: the result of an activity one app starts in another for a result (startActivityForResult with another app's
// activity, which the shell starts in that app's own process: megler.PhoneApps). Play Store starts microG's sign-in so and
// goes on when it hears RESULT_OK.
//
// On Android the activity runs in the caller's task, and as it finishes its result comes to the caller, which is in front
// again. Here the two apps are two processes, whose one shared place is the services' folder every app has
// (/data/megler-services, core/src/run/mg_run.c). So:
//  - the caller notes there, in results/<package>/<activity>.caller, that it waits for that activity (its own package), and
//    keeps which of its activities asked and with which request code;
//  - the app the activity is in, when an activity another app asked for (AppRunner.startRequested) finishes, writes its result
//    beside that note (<activity>.json: the result code, and the intent's action, data and extras of plain types) and brings
//    the caller to the front, as finishing the activity shows the caller's task on Android;
//  - the caller, in front again, gives the result to the activity that asked (onActivityResult, before it resumes). With none
//    written yet (the user came back without finishing the other app's screen) it goes on waiting.
package megler;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import org.json.JSONObject;

public final class AppResults {
    private AppResults() {}
    private static final String TAG = "Megler";

    /** What the caller waits for. */
    private static final class Awaited {
        Activity caller;
        int request;
        File result;
        String what;
    }
    private static Awaited sAwaited;
    /** The activities of this app another app asked for, by class: their results are written as they finish. */
    private static final HashSet<String> sRequested = new HashSet<>();

    /** The folder of the results of `pkg`'s activities, or null with no services' folder. */
    private static File folder(String pkg) {
        File shared = new File(MicroG.SHARED);
        return shared.isDirectory() ? new File(new File(shared, "results"), pkg) : null;
    }

    // ---- the caller's side

    /** `caller` asked for the result of `pkg`'s `activity` (request code `request`), which the shell is starting. */
    static void await(Activity caller, int request, String pkg, String activity) {
        File dir = folder(pkg);
        if (caller == null || request < 0 || activity == null || dir == null) return;
        dir.mkdirs();
        Awaited w = new Awaited();
        w.caller = caller;
        w.request = request;
        w.result = new File(dir, activity + ".json");
        w.what = pkg + "/" + activity;
        // an earlier result is not this request's
        w.result.delete();
        if (!write(new File(dir, activity + ".caller"), Native.packageName())) return;
        sAwaited = w;
        Log.i(TAG, "phone apps: " + caller.getClass().getName() + " waits for the result of " + w.what + " (request " + request + ")");
    }

    /** This app is in front again: the result, when the other app wrote it, goes to the activity that asked. */
    static void resumed() {
        Awaited w = sAwaited;
        if (w == null) return;
        if (!w.result.isFile()) {
            Log.i(TAG, "phone apps: " + w.what + " has not finished; still waiting for its result");
            return;
        }
        sAwaited = null;
        String text = read(w.result);
        w.result.delete();
        if (w.caller.isFinishing() || w.caller.isDestroyed()) return;
        int code = Activity.RESULT_CANCELED;
        Intent data = null;
        try {
            JSONObject o = new JSONObject(text);
            code = o.getInt("code");
            if (o.has("intent")) data = intent(o.getJSONObject("intent"));
        } catch (Exception e) {
            Log.w(TAG, "phone apps: the result of " + w.what + " cannot be read: " + e);
        }
        Log.i(TAG, "phone apps: the result of " + w.what + ": " + code + (data != null ? " " + data : ""));
        w.caller.meglerActivityResult(w.request, code, data);
    }

    // ---- the side of the app the activity is in

    /** Another app asked for this app's activity `cls`. */
    static void requested(String cls) {
        synchronized (sRequested) { sRequested.add(cls); }
    }

    /** `a` finishes: when another app asked for it and waits for it, its result is written and that app comes to the front. */
    static void finished(Activity a) {
        String cls = a.getClass().getName();
        synchronized (sRequested) { if (!sRequested.remove(cls)) return; }
        File dir = folder(Native.packageName());
        if (dir == null) return;
        File note = new File(dir, cls + ".caller");
        String caller = note.isFile() ? read(note).trim() : "";
        note.delete();
        if (caller.isEmpty()) return;
        try {
            JSONObject o = new JSONObject();
            o.put("code", a.meglerResultCode());
            Intent data = a.meglerResultData();
            if (data != null) o.put("intent", json(data));
            if (!write(new File(dir, cls + ".json"), o.toString())) return;
        } catch (Exception e) {
            Log.w(TAG, "phone apps: the result of " + cls + " for " + caller + " cannot be written: " + e);
            return;
        }
        Log.i(TAG, "phone apps: " + cls + " finished with " + a.meglerResultCode() + "; back to " + caller);
        if (!Native.launchApp(caller, null)) Log.w(TAG, "phone apps: " + caller + " was not brought back");
    }

    // ---- an intent as JSON: its action, data and the extras of plain types

    private static JSONObject json(Intent i) throws Exception {
        JSONObject o = new JSONObject();
        if (i.getAction() != null) o.put("action", i.getAction());
        if (i.getData() != null) o.put("data", i.getData().toString());
        Bundle b = i.getExtras();
        if (b != null) {
            JSONObject extras = new JSONObject();
            for (String k : b.keySet()) {
                Object v = b.get(k);
                String t = v instanceof String ? "s" : v instanceof Integer ? "i" : v instanceof Long ? "l" : v instanceof Boolean ? "b" : v instanceof Double ? "d" : null;
                if (t == null) continue;
                extras.put(k, new JSONObject().put("t", t).put("v", v));
            }
            o.put("extras", extras);
        }
        return o;
    }

    private static Intent intent(JSONObject o) throws Exception {
        Intent i = new Intent();
        if (o.has("action")) i.setAction(o.getString("action"));
        if (o.has("data")) i.setData(android.net.Uri.parse(o.getString("data")));
        JSONObject extras = o.optJSONObject("extras");
        if (extras != null) {
            java.util.Iterator<String> keys = extras.keys();
            while (keys.hasNext()) {
                String k = keys.next();
                JSONObject e = extras.getJSONObject(k);
                String t = e.getString("t");
                if (t.equals("s")) i.putExtra(k, e.getString("v"));
                else if (t.equals("i")) i.putExtra(k, e.getInt("v"));
                else if (t.equals("l")) i.putExtra(k, e.getLong("v"));
                else if (t.equals("b")) i.putExtra(k, e.getBoolean("v"));
                else if (t.equals("d")) i.putExtra(k, e.getDouble("v"));
            }
        }
        return i;
    }

    private static boolean write(File f, String text) {
        File tmp = new File(f.getPath() + "." + android.os.Process.myPid() + ".new");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "phone apps: " + f + " cannot be written: " + e);
            return false;
        }
        if (tmp.renameTo(f)) return true;
        f.delete();
        return tmp.renameTo(f);
    }

    private static String read(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (Exception e) {
            return "";
        }
    }
}
