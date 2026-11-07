// Megler: the phone's text messages and calls, for an app as Android has them (core/include/megler.h: megler_sms_received,
// megler_call_logged; the shell's player/PhoneEvents.cpp reads them from Windows).
//
// Why. WhatsApp confirms a phone number by a text message or by a call: Google Play services' SMS Retriever hears the
// message through Android's SMS_RECEIVED and hands WhatsApp its code, and WhatsApp's FlashCallReceiver hears the call
// through PHONE_STATE RINGING with the caller's number, which is the code. Under Megler neither came: the phone had the
// message and the call, and the app never heard of them.
//
// What an app is given is what came while it ran, nothing older, and as Android gives it to an app that holds the
// permission (the app's manifest asks for it and the user let it have it: megler.Permissions): SMS_RECEIVED to receivers that hold
// RECEIVE_SMS, content://sms to READ_SMS, PHONE_STATE to READ_PHONE_STATE (the number to READ_CALL_LOG as well), and
// content://call_log to READ_CALL_LOG. Google Play services, which an app hosts here, holds them as it does on a phone.
package megler;

import android.app.PendingIntent;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.TimeZone;

public final class Phone {
    private Phone() {}

    public static final String RECEIVE_SMS = "android.permission.RECEIVE_SMS", READ_SMS = "android.permission.READ_SMS",
        READ_PHONE_STATE = "android.permission.READ_PHONE_STATE", READ_CALL_LOG = "android.permission.READ_CALL_LOG";
    private static final Uri SMS_URI = Uri.parse("content://sms"), CALLS_URI = Uri.parse("content://call_log/calls");

    // ---- what the shell sends

    /** The phone has something for the app (an EV_PHONE event, core/src/drivers/mg_java.c): taken and told, on the main thread. */
    public static void deliver() {
        String[] items = Native.pollPhone();
        if (items == null || items.length == 0 || items[0] == null || items[0].isEmpty()) return;
        String it = items[0];
        try {
            String[] f = it.substring(1).split("\n", it.charAt(0) == 's' ? 3 : 4);
            if (it.charAt(0) == 's' && f.length == 3) smsArrived(Long.parseLong(f[0]), f[1], f[2]);
            else if (it.charAt(0) == 'c' && f.length == 4) callLogged(Long.parseLong(f[0]), Integer.parseInt(f[1]), Integer.parseInt(f[2]), f[3]);
        } catch (RuntimeException e) {
            android.util.Log.w("Megler", "phone: what the shell sent could not be told to the app: " + e);
        }
    }

    // ---- who holds what

    /** Whether the package of a context holds a permission: the app as its manifest asks and the user let it (megler.Permissions:
     *  RECEIVE_SMS and the rest are dangerous), Google Play services as it does on a phone. */
    public static boolean holds(Context c, String permission) {
        if (permission == null) return true;
        String pkg = c != null ? c.getPackageName() : null;
        if (GooglePackages.GMS.equals(pkg)) return true;
        return Permissions.check(permission) == Permissions.GRANTED;
    }

    // ---- the store: what came while the app ran

    private static SQLiteDatabase sDb;

    private static synchronized SQLiteDatabase db() {
        if (sDb != null) return sDb;
        sDb = SQLiteDatabase.create(null);
        // Android's columns of Telephony.Sms and CallLog.Calls, so that an app's projection and selection find what they name.
        sDb.execSQL("CREATE TABLE sms (_id INTEGER PRIMARY KEY AUTOINCREMENT, thread_id INTEGER DEFAULT 1, address TEXT, person INTEGER,"
            + " date INTEGER, date_sent INTEGER, protocol INTEGER DEFAULT 0, read INTEGER DEFAULT 0, status INTEGER DEFAULT -1,"
            + " type INTEGER DEFAULT 1, reply_path_present INTEGER DEFAULT 0, subject TEXT, body TEXT, service_center TEXT,"
            + " locked INTEGER DEFAULT 0, error_code INTEGER DEFAULT 0, seen INTEGER DEFAULT 0, sub_id INTEGER DEFAULT 1, creator TEXT)");
        sDb.execSQL("CREATE TABLE calls (_id INTEGER PRIMARY KEY AUTOINCREMENT, number TEXT, presentation INTEGER DEFAULT 1,"
            + " post_dial_digits TEXT DEFAULT '', via_number TEXT DEFAULT '', date INTEGER, duration INTEGER, data_usage INTEGER,"
            + " type INTEGER, features INTEGER DEFAULT 0, subscription_component_name TEXT, subscription_id TEXT, phone_account_address TEXT,"
            + " phone_account_hidden INTEGER DEFAULT 0, add_for_all_users INTEGER DEFAULT 1, new INTEGER DEFAULT 1, name TEXT,"
            + " numbertype INTEGER, numberlabel TEXT, countryiso TEXT, voicemail_uri TEXT, is_read INTEGER DEFAULT 0,"
            + " geocoded_location TEXT, lookup_uri TEXT, matched_number TEXT, normalized_number TEXT, photo_id INTEGER DEFAULT 0,"
            + " photo_uri TEXT, formatted_number TEXT, transcription TEXT, transcription_state INTEGER DEFAULT 0, last_modified INTEGER,"
            + " block_reason INTEGER DEFAULT 0, call_screening_component_name TEXT, call_screening_app_name TEXT, priority INTEGER DEFAULT 0,"
            + " subject TEXT, location TEXT, composer_photo_uri TEXT, missed_reason INTEGER DEFAULT 0, is_call_log_phone_account_migration_pending INTEGER DEFAULT 0)");
        return sDb;
    }

    // ---- text messages

    /** A text message the phone has: kept, broadcast as SMS_RECEIVED, and sent to an app-specific SMS token's PendingIntent. */
    static void smsArrived(long time, String from, String body) {
        ContentValues v = new ContentValues();
        v.put("address", from); v.put("date", time); v.put("date_sent", time); v.put("body", body); v.put("type", 1);
        db().insert("sms", null, v);
        Context app = AppRunner.application();
        if (app != null) app.getContentResolver().notifyChange(SMS_URI, null);
        Intent i = smsIntent(from, body, time);
        Broadcasts.send(app, i, RECEIVE_SMS);
        PendingIntent token = tokenFor(body);
        if (token != null) {
            try { token.send(app, 0, new Intent(i)); }
            catch (PendingIntent.CanceledException e) { android.util.Log.i("Megler", "phone: the app-specific SMS token's PendingIntent was cancelled"); }
        }
        android.util.Log.i("Megler", "phone: a text message (" + body.length() + " characters) to the app as SMS_RECEIVED"
            + (token != null ? ", and to the PendingIntent of the app-specific SMS token in it" : ""));
    }

    /** SMS_RECEIVED as the phone's telephony sends it: the message in 3GPP PDUs (one, or a concatenated message's parts), "format" 3gpp. */
    public static Intent smsIntent(String from, String body, long time) {
        Intent i = new Intent("android.provider.Telephony.SMS_RECEIVED");
        byte[][] pdus = pdus(from, body, time);
        Object[] objects = new Object[pdus.length];
        System.arraycopy(pdus, 0, objects, 0, pdus.length);
        i.putExtra("pdus", objects);
        i.putExtra("format", "3gpp");
        i.putExtra("subscription", 1);
        i.putExtra("android.telephony.extra.SUBSCRIPTION_INDEX", 1);
        i.putExtra("slot", 0);
        i.putExtra("phone", 0);
        return i;
    }

    private static int sRef;

    /**
     * SMS-DELIVER PDUs of a message (3GPP TS 23.040): no service centre, the sender as an international or national number or
     * as GSM 7-bit letters, the time in UTC, the text in UCS-2 -- in parts of 67 characters with a concatenation header when it
     * is longer than 70.
     */
    public static byte[][] pdus(String from, String body, long time) {
        if (body == null) body = "";
        int parts = body.length() <= 70 ? 1 : (body.length() + 66) / 67;
        int ref = (sRef++) & 0xFF;
        byte[][] out = new byte[parts][];
        for (int k = 0; k < parts; k++) {
            String piece = parts == 1 ? body : body.substring(k * 67, Math.min(body.length(), (k + 1) * 67));
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            b.write(0);                                         // no service centre address
            b.write(0x04 | (parts > 1 ? 0x40 : 0));             // SMS-DELIVER, no more messages, a header when in parts
            address(b, from == null ? "" : from);
            b.write(0);                                         // protocol identifier
            b.write(0x08);                                      // UCS-2
            timestamp(b, time);
            int header = parts > 1 ? 6 : 0;
            b.write(header + piece.length() * 2);
            if (parts > 1) { b.write(5); b.write(0); b.write(3); b.write(ref); b.write(parts); b.write(k + 1); }
            for (int c = 0; c < piece.length(); c++) { char ch = piece.charAt(c); b.write(ch >> 8); b.write(ch & 0xFF); }
            out[k] = b.toByteArray();
        }
        return out;
    }

    private static void address(java.io.ByteArrayOutputStream b, String a) {
        String digits = a.startsWith("+") ? a.substring(1) : a;
        boolean number = !digits.isEmpty() && digits.matches("[0-9*#]+");
        if (number) {
            b.write(digits.length());
            b.write(a.startsWith("+") ? 0x91 : 0x81);
            for (int i = 0; i < digits.length(); i += 2) {
                int lo = semiOctet(digits.charAt(i)), hi = i + 1 < digits.length() ? semiOctet(digits.charAt(i + 1)) : 0xF;
                b.write(hi << 4 | lo);
            }
            return;
        }
        // An alphanumeric sender, in GSM 7-bit (11 characters at most): its length in semi-octets
        String name = a.length() > 11 ? a.substring(0, 11) : a;
        byte[] packed = SmsCodec.pack7(name);
        b.write((name.length() * 7 + 3) / 4);
        b.write(0xD0);
        b.write(packed, 0, packed.length);
    }

    private static int semiOctet(char c) { return c == '*' ? 0xA : c == '#' ? 0xB : c - '0'; }

    private static void timestamp(java.io.ByteArrayOutputStream b, long time) {
        java.util.Calendar c = java.util.Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        c.setTimeInMillis(time);
        int[] v = { c.get(java.util.Calendar.YEAR) % 100, c.get(java.util.Calendar.MONTH) + 1, c.get(java.util.Calendar.DAY_OF_MONTH),
                    c.get(java.util.Calendar.HOUR_OF_DAY), c.get(java.util.Calendar.MINUTE), c.get(java.util.Calendar.SECOND), 0 };
        for (int x : v) b.write((x % 10) << 4 | (x / 10));
    }

    // ---- app-specific SMS tokens (SmsManager.createAppSpecificSmsToken)

    private static final HashMap<String, Object[]> sTokens = new HashMap<>();

    /** A token that, in a text message that comes, sends `intent` with the message instead of only broadcasting it. */
    public static String token(PendingIntent intent, String prefixes) {
        if (intent == null) return null;
        String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        java.security.SecureRandom r = new java.security.SecureRandom();
        StringBuilder t = new StringBuilder();
        for (int i = 0; i < 11; i++) t.append(alphabet.charAt(r.nextInt(alphabet.length())));
        synchronized (sTokens) { sTokens.put(t.toString(), new Object[] { intent, prefixes }); }
        return t.toString();
    }

    private static PendingIntent tokenFor(String body) {
        synchronized (sTokens) {
            for (java.util.Iterator<java.util.Map.Entry<String, Object[]>> it = sTokens.entrySet().iterator(); it.hasNext(); ) {
                java.util.Map.Entry<String, Object[]> e = it.next();
                if (!body.contains(e.getKey())) continue;
                String prefixes = (String) e.getValue()[1];
                boolean prefixed = prefixes == null || prefixes.isEmpty();
                if (!prefixed) for (String p : prefixes.split(";")) if (!p.isEmpty() && body.startsWith(p)) prefixed = true;
                if (!prefixed) continue;
                it.remove();                                    // a token is good for one message, as on Android
                return (PendingIntent) e.getValue()[0];
            }
        }
        return null;
    }

    // ---- calls

    private static int sCallState;
    private static final ArrayList<Object[]> sListeners = new ArrayList<>();   // { PhoneStateListener or CallStateListener, Executor or null }

    public static int callState() { return sCallState; }

    /** TelephonyManager.listen(LISTEN_CALL_STATE), registerTelephonyCallback with a CallStateListener: told what follows. */
    public static void listen(Object listener, java.util.concurrent.Executor on, boolean add) {
        synchronized (sListeners) {
            for (int i = sListeners.size() - 1; i >= 0; i--) if (sListeners.get(i)[0] == listener) sListeners.remove(i);
            if (add) sListeners.add(new Object[] { listener, on });
        }
    }

    /** A call the phone's history has: kept; one that came in is told as a call that rang (and was answered, or was not). */
    static void callLogged(long time, int type, int duration, String number) {
        ContentValues v = new ContentValues();
        v.put("number", number); v.put("date", time); v.put("duration", duration); v.put("type", type);
        v.put("new", type == 3 ? 1 : 0); v.put("last_modified", System.currentTimeMillis());
        v.put("normalized_number", number); v.put("formatted_number", number);
        db().insert("calls", null, v);
        Context app = AppRunner.application();
        if (app != null) app.getContentResolver().notifyChange(CALLS_URI, null);
        boolean incoming = type == 1 || type == 3 || type == 5 || type == 6 || type == 7;
        android.util.Log.i("Megler", "phone: a call of the phone's history (type " + type + ") to the app" + (incoming ? ", told as a call that rang" : ""));
        if (!incoming) return;
        state(1, number);
        Handler main = new Handler(Looper.getMainLooper());
        if (type == 1) main.postDelayed(() -> state(2, number), 500);
        main.postDelayed(() -> state(0, number), type == 1 ? 1000 : 700);
    }

    /** The call state as Android tells it: PHONE_STATE (the number to those that may read it) and the listeners. */
    private static void state(int state, String number) {
        sCallState = state;
        Context app = AppRunner.application();
        String name = state == 1 ? "RINGING" : state == 2 ? "OFFHOOK" : "IDLE";
        // Android 9 and later send it twice: with the number to receivers that hold READ_CALL_LOG, and without to the others.
        Intent with = new Intent("android.intent.action.PHONE_STATE").putExtra("state", name).putExtra("incoming_number", number);
        Intent without = new Intent("android.intent.action.PHONE_STATE").putExtra("state", name);
        Broadcasts.send(app, with, READ_PHONE_STATE, READ_CALL_LOG, true);
        Broadcasts.send(app, without, READ_PHONE_STATE, READ_CALL_LOG, false);
        final boolean numbered = holds(app, READ_CALL_LOG);
        ArrayList<Object[]> targets;
        synchronized (sListeners) { targets = new ArrayList<>(sListeners); }
        Handler main = new Handler(Looper.getMainLooper());
        for (Object[] t : targets) {
            final Object l = t[0];
            Runnable tell = () -> {
                if (l instanceof android.telephony.PhoneStateListener) ((android.telephony.PhoneStateListener) l).onCallStateChanged(state, numbered ? number : "");
                else if (l instanceof android.telephony.TelephonyCallback.CallStateListener) ((android.telephony.TelephonyCallback.CallStateListener) l).onCallStateChanged(state);
            };
            if (t[1] != null) ((java.util.concurrent.Executor) t[1]).execute(tell);
            else main.post(tell);
        }
    }

    // ---- content://sms and content://call_log

    public static final ContentProvider SMS = new Provider(true), CALL_LOG = new Provider(false);

    private static final class Provider extends ContentProvider {
        private final boolean sms;
        Provider(boolean sms) { this.sms = sms; }
        @Override public boolean onCreate() { return true; }
        @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
            Context app = AppRunner.application();
            if (!holds(app, sms ? READ_SMS : READ_CALL_LOG)) return new MatrixCursor(projection != null ? projection : new String[] { "_id" });
            List<String> path = uri.getPathSegments();
            String where = selection;
            String id = null, folder = null;
            if (sms) {
                if (!path.isEmpty()) { if (path.get(0).matches("[0-9]+")) id = path.get(0); else folder = path.get(0); }
                if (path.size() > 1 && path.get(1).matches("[0-9]+")) id = path.get(1);
                if (folder != null && !"inbox".equals(folder)) where = and(where, "type=" + folderType(folder));
                else if ("inbox".equals(folder)) where = and(where, "type=1");
            } else if (path.size() > 1 && path.get(1).matches("[0-9]+")) id = path.get(1);
            if (id != null) where = and(where, "_id=" + id);
            String limit = uri.getQueryParameter("limit");
            try {
                return db().query(sms ? "sms" : "calls", projection, where, args, null, null, order == null || order.isEmpty() ? "date DESC" : order,
                                  limit != null && limit.matches("[0-9]+") ? limit : null);
            } catch (RuntimeException e) {
                android.util.Log.w("Megler", "phone: a query of " + uri + " the store cannot answer (" + e.getMessage() + "): no rows");
                return new MatrixCursor(projection != null ? projection : new String[] { "_id" });
            }
        }
        private static String and(String a, String b) { return a == null || a.trim().isEmpty() ? b : "(" + a + ") AND " + b; }
        private static int folderType(String f) {
            switch (f) { case "sent": return 2; case "draft": return 3; case "outbox": return 4; case "failed": return 5; case "queued": return 6; default: return -1; }
        }
        @Override public String getType(Uri uri) { return sms ? "vnd.android.cursor.dir/sms" : "vnd.android.cursor.dir/calls"; }
        @Override public Uri insert(Uri uri, ContentValues v) { return null; }
        @Override public int delete(Uri uri, String s, String[] a) { return 0; }
        @Override public int update(Uri uri, ContentValues v, String s, String[] a) {
            // what an app may change of what it was given: read and seen marks
            if (v == null || !holds(AppRunner.application(), sms ? READ_SMS : READ_CALL_LOG)) return 0;
            ContentValues keep = new ContentValues();
            for (String k : new String[] { "read", "seen", "new", "is_read" }) if (v.containsKey(k)) keep.put(k, v.getAsInteger(k));
            if (keep.size() == 0) return 0;
            try { return db().update(sms ? "sms" : "calls", keep, s, a); } catch (RuntimeException e) { return 0; }
        }
    }
}
