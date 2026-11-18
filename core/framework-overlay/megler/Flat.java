// Megler: an Intent as bytes another app can read -- another Windows process, with classes of its own.
//
// Why not a Parcel's. Megler's Parcel (android.os.Parcel, overlaid) keeps what is not a number or a string -- a Bundle, a
// Parcelable, a list -- in a table of this process's objects, and its bytes hold only their places in it: marshalled and read
// in another process, every one of them is null. So an Intent that goes to another app (megler.PhoneApps.start, the file the
// other app reads at megler.AppRunner.startRequested) or comes back from it as a result (megler.AppResults) is written out
// here, value by value: its action, data, type, package, component, flags, categories, identifier, selector, clip data and
// its extras -- strings, numbers, their arrays, Bundles, lists, maps, Uris, Intents, Serializables, and any other Parcelable
// by its class's name, the bytes its writeToParcel gives and the table those bytes point into (each object of it written the
// same way). The reader makes a Parcelable with the CREATOR of its own class of that name, as Android's receiver does.
//
// What cannot go, as on Android or because there is no other process to reach back into: a Binder, a file descriptor, a
// PendingIntent (Megler's is this process's object), and a Parcelable of a class the receiver does not have. Such a value is
// left out, with a line in the log, and the rest of the Intent goes.
//
// Total Commander showed the need: it hands its Wi-Fi plug-in the files to send as an Intent with an action, a type, a
// category and a list of Uris among the extras, of which the plug-in got the activity's class alone.
package megler;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ComponentName;
import android.content.Intent;
import android.net.Uri;
import android.os.BaseBundle;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

public final class Flat {
    private Flat() {}
    private static final String TAG = "Megler";
    private static final int MAGIC = 0x4D674946, VERSION = 1;
    /* How deep values may hold values: an Intent's extras are a few levels; a structure that holds itself ends here. */
    private static final int DEPTH = 24;
    private static final int NULL = 0, STRING = 1, INT = 2, LONG = 3, BOOLEAN = 4, DOUBLE = 5, FLOAT = 6, SHORT = 7, BYTE = 8, CHAR = 9,
        BYTES = 10, INTS = 11, LONGS = 12, BOOLEANS = 13, DOUBLES = 14, FLOATS = 15, SHORTS = 16, CHARS = 17, STRINGS = 18, TEXTS = 19,
        BUNDLE = 20, PERSISTABLE = 21, INTENT = 22, COMPONENT = 23, URI = 24, LIST = 25, MAP = 26, PARCELABLES = 27, OBJECTS = 28,
        SPARSE = 29, SERIALIZABLE = 30, PARCELABLE = 31;
    /* Read: a value the receiver cannot have (a Parcelable of a class it lacks): left out of what held it. */
    private static final Object DROPPED = new Object();
    /* Written: a value that cannot go to another process. */
    private static final class NotFlat extends Exception {
        NotFlat(String why) { super(why, null, false, false); }
    }

    /** The Intent as bytes; never null. */
    public static byte[] intent(Intent i) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream(256);
            DataOutputStream out = new DataOutputStream(bytes);
            out.writeInt(MAGIC);
            out.writeInt(VERSION);
            writeIntent(out, i, 0);
            out.flush();
            return bytes.toByteArray();
        } catch (IOException e) {
            throw new IllegalStateException(e);   // (a stream in memory does not fail)
        }
    }

    /** The Intent those bytes are, its Parcelables made by `l`'s classes; null for bytes that are not an Intent's. */
    public static Intent intent(byte[] bytes, ClassLoader l) {
        if (bytes == null) return null;
        try {
            DataInputStream in = new DataInputStream(new ByteArrayInputStream(bytes));
            if (in.readInt() != MAGIC || in.readInt() != VERSION) return null;
            return readIntent(in, l != null ? l : Flat.class.getClassLoader(), 0);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "an intent from another app cannot be read: " + e);
            return null;
        }
    }

    // ---- an Intent

    private static void writeIntent(DataOutputStream out, Intent i, int depth) throws IOException {
        str(out, i.getAction());
        str(out, i.getData() == null ? null : i.getData().toString());
        str(out, i.getType());
        str(out, i.getPackage());
        str(out, i.getComponent() == null ? null : i.getComponent().flattenToString());
        out.writeInt(i.getFlags());
        java.util.Set<String> categories = i.getCategories();
        out.writeInt(categories == null ? 0 : categories.size());
        if (categories != null) for (String c : categories) str(out, c);
        str(out, i.getIdentifier());
        Intent selector = depth < DEPTH ? i.getSelector() : null;
        out.writeBoolean(selector != null);
        if (selector != null) writeIntent(out, selector, depth + 1);
        ClipData clip = depth < DEPTH ? i.getClipData() : null;
        out.writeBoolean(clip != null);
        if (clip != null) writeClip(out, clip, depth + 1);
        Bundle extras = i.getExtras();
        out.writeBoolean(extras != null);
        if (extras != null) writeBundle(out, extras, depth + 1);
    }

    private static Intent readIntent(DataInputStream in, ClassLoader l, int depth) throws IOException {
        Intent i = new Intent();
        i.setAction(str(in));
        String data = str(in), type = str(in);
        if (data != null || type != null) i.setDataAndType(data == null ? null : Uri.parse(data), type);
        i.setPackage(str(in));
        String component = str(in);
        if (component != null) i.setComponent(ComponentName.unflattenFromString(component));
        i.setFlags(in.readInt());
        for (int n = count(in); n > 0; n--) i.addCategory(str(in));
        String identifier = str(in);
        if (identifier != null) i.setIdentifier(identifier);
        if (in.readBoolean()) i.setSelector(readIntent(in, l, depth + 1));
        if (in.readBoolean()) i.setClipData(readClip(in, l, depth + 1));
        if (in.readBoolean()) {
            Bundle extras = new Bundle();
            readBundle(in, extras, l, depth + 1);
            i.replaceExtras(extras);
        }
        return i;
    }

    /* Clip data: its label and types, and each item's text, HTML, Uri and Intent. */
    private static void writeClip(DataOutputStream out, ClipData clip, int depth) throws IOException {
        ClipDescription d = clip.getDescription();
        str(out, d == null || d.getLabel() == null ? null : d.getLabel().toString());
        int types = d == null ? 0 : d.getMimeTypeCount();
        out.writeInt(types);
        for (int k = 0; k < types; k++) str(out, d.getMimeType(k));
        out.writeInt(clip.getItemCount());
        for (int k = 0; k < clip.getItemCount(); k++) {
            ClipData.Item item = clip.getItemAt(k);
            str(out, item.getText() == null ? null : item.getText().toString());
            str(out, item.getHtmlText());
            str(out, item.getUri() == null ? null : item.getUri().toString());
            Intent intent = depth < DEPTH ? item.getIntent() : null;
            out.writeBoolean(intent != null);
            if (intent != null) writeIntent(out, intent, depth + 1);
        }
    }

    private static ClipData readClip(DataInputStream in, ClassLoader l, int depth) throws IOException {
        String label = str(in);
        String[] types = new String[count(in)];
        for (int k = 0; k < types.length; k++) types[k] = str(in);
        ClipData clip = null;
        for (int n = count(in); n > 0; n--) {
            String text = str(in), html = str(in), uri = str(in);
            Intent intent = in.readBoolean() ? readIntent(in, l, depth + 1) : null;
            ClipData.Item item = new ClipData.Item(text, html, intent, uri == null ? null : Uri.parse(uri));
            if (clip == null) clip = new ClipData(label, types, item); else clip.addItem(item);
        }
        return clip;
    }

    // ---- a bundle: each value written apart, so that one that cannot go leaves the others whole

    private static void writeBundle(DataOutputStream out, BaseBundle b, int depth) throws IOException {
        ArrayList<String> keys = new ArrayList<>();
        ArrayList<byte[]> values = new ArrayList<>();
        for (String key : new ArrayList<>(b.keySet())) {
            Object v = b.get(key);
            ByteArrayOutputStream one = new ByteArrayOutputStream(64);
            try {
                writeValue(new DataOutputStream(one), v, depth);
            } catch (NotFlat e) {
                Log.w(TAG, "an intent for another app: its extra " + key + " (" + v.getClass().getName() + ") stays here: " + e.getMessage());
                continue;
            }
            keys.add(key);
            values.add(one.toByteArray());
        }
        out.writeInt(keys.size());
        for (int k = 0; k < keys.size(); k++) { str(out, keys.get(k)); out.write(values.get(k)); }
    }

    private static void readBundle(DataInputStream in, BaseBundle b, ClassLoader l, int depth) throws IOException {
        for (int n = count(in); n > 0; n--) {
            String key = str(in);
            Object v = readValue(in, l, depth);
            if (v != DROPPED) b.putObject(key, v);
        }
    }

    // ---- a value

    private static void writeValue(DataOutputStream out, Object v, int depth) throws IOException, NotFlat {
        if (depth > DEPTH) throw new NotFlat("it is nested too deep");
        if (v == null) { out.writeByte(NULL); return; }
        if (v instanceof String) { out.writeByte(STRING); str(out, (String) v); }
        else if (v instanceof Integer) { out.writeByte(INT); out.writeInt((Integer) v); }
        else if (v instanceof Long) { out.writeByte(LONG); out.writeLong((Long) v); }
        else if (v instanceof Boolean) { out.writeByte(BOOLEAN); out.writeBoolean((Boolean) v); }
        else if (v instanceof Double) { out.writeByte(DOUBLE); out.writeDouble((Double) v); }
        else if (v instanceof Float) { out.writeByte(FLOAT); out.writeFloat((Float) v); }
        else if (v instanceof Short) { out.writeByte(SHORT); out.writeShort((Short) v); }
        else if (v instanceof Byte) { out.writeByte(BYTE); out.writeByte((Byte) v); }
        else if (v instanceof Character) { out.writeByte(CHAR); out.writeChar((Character) v); }
        else if (v instanceof byte[]) { byte[] a = (byte[]) v; out.writeByte(BYTES); out.writeInt(a.length); out.write(a); }
        else if (v instanceof int[]) { int[] a = (int[]) v; out.writeByte(INTS); out.writeInt(a.length); for (int x : a) out.writeInt(x); }
        else if (v instanceof long[]) { long[] a = (long[]) v; out.writeByte(LONGS); out.writeInt(a.length); for (long x : a) out.writeLong(x); }
        else if (v instanceof boolean[]) { boolean[] a = (boolean[]) v; out.writeByte(BOOLEANS); out.writeInt(a.length); for (boolean x : a) out.writeBoolean(x); }
        else if (v instanceof double[]) { double[] a = (double[]) v; out.writeByte(DOUBLES); out.writeInt(a.length); for (double x : a) out.writeDouble(x); }
        else if (v instanceof float[]) { float[] a = (float[]) v; out.writeByte(FLOATS); out.writeInt(a.length); for (float x : a) out.writeFloat(x); }
        else if (v instanceof short[]) { short[] a = (short[]) v; out.writeByte(SHORTS); out.writeInt(a.length); for (short x : a) out.writeShort(x); }
        else if (v instanceof char[]) { char[] a = (char[]) v; out.writeByte(CHARS); out.writeInt(a.length); for (char x : a) out.writeChar(x); }
        else if (v instanceof String[]) { String[] a = (String[]) v; out.writeByte(STRINGS); out.writeInt(a.length); for (String x : a) str(out, x); }
        else if (v instanceof CharSequence[]) {
            CharSequence[] a = (CharSequence[]) v;
            out.writeByte(TEXTS); out.writeInt(a.length);
            for (CharSequence x : a) str(out, x == null ? null : x.toString());
        }
        // (text with spans goes as its characters)
        else if (v instanceof CharSequence) { out.writeByte(STRING); str(out, v.toString()); }
        else if (v instanceof Bundle) { out.writeByte(BUNDLE); writeBundle(out, (Bundle) v, depth + 1); }
        else if (v instanceof PersistableBundle) { out.writeByte(PERSISTABLE); writeBundle(out, (PersistableBundle) v, depth + 1); }
        else if (v instanceof Intent) { out.writeByte(INTENT); writeIntent(out, (Intent) v, depth + 1); }
        else if (v instanceof ComponentName) { out.writeByte(COMPONENT); str(out, ((ComponentName) v).flattenToString()); }
        else if (v instanceof Uri) { out.writeByte(URI); str(out, v.toString()); }
        else if (v instanceof java.util.List) {
            java.util.List<?> a = (java.util.List<?>) v;
            out.writeByte(LIST); out.writeInt(a.size());
            for (Object x : a) writeValue(out, x, depth + 1);
        }
        else if (v instanceof Map) {
            Map<?, ?> m = (Map<?, ?>) v;
            out.writeByte(MAP); out.writeInt(m.size());
            for (Map.Entry<?, ?> e : m.entrySet()) { writeValue(out, e.getKey(), depth + 1); writeValue(out, e.getValue(), depth + 1); }
        }
        else if (v instanceof Object[] && (v instanceof Parcelable[] || v.getClass() == Object[].class)) {
            Object[] a = (Object[]) v;
            out.writeByte(v instanceof Parcelable[] ? PARCELABLES : OBJECTS); out.writeInt(a.length);
            for (Object x : a) writeValue(out, x, depth + 1);
        }
        else if (v instanceof android.util.SparseArray) {
            android.util.SparseArray<?> a = (android.util.SparseArray<?>) v;
            out.writeByte(SPARSE); out.writeInt(a.size());
            for (int k = 0; k < a.size(); k++) { out.writeInt(a.keyAt(k)); writeValue(out, a.valueAt(k), depth + 1); }
        }
        else if (v instanceof android.os.IBinder || v instanceof android.os.IInterface) throw new NotFlat("a Binder reaches no other app's process");
        else if (v instanceof java.io.FileDescriptor || v instanceof android.os.ParcelFileDescriptor) throw new NotFlat("a file descriptor reaches no other app's process");
        else if (v instanceof Parcelable) writeParcelable(out, (Parcelable) v, depth);
        else if (v instanceof java.io.Serializable) {
            ByteArrayOutputStream b = new ByteArrayOutputStream(128);
            try (java.io.ObjectOutputStream o = new java.io.ObjectOutputStream(b)) {
                o.writeObject(v);
            } catch (IOException | RuntimeException e) {
                throw new NotFlat("it cannot be serialized (" + e + ")");
            }
            out.writeByte(SERIALIZABLE); out.writeInt(b.size()); b.writeTo(out);
        }
        else throw new NotFlat("nothing writes its class out");
    }

    /* A Parcelable of any class: its class's name, the bytes it writes and the objects those bytes point to. */
    private static void writeParcelable(DataOutputStream out, Parcelable v, int depth) throws IOException, NotFlat {
        Parcel p = Parcel.obtain();
        try {
            byte[] bytes;
            Object[] table;
            try {
                v.writeToParcel(p, 0);
                bytes = p.meglerBytes();
                table = p.meglerObjects();
            } catch (RuntimeException e) {
                throw new NotFlat("it cannot be written (" + e + ")");
            }
            // (Megler's PendingIntent and IntentSender write themselves so: objects of this process)
            for (Object o : table) if (o == v) throw new NotFlat("it is an object of this process only");
            out.writeByte(PARCELABLE);
            str(out, v.getClass().getName());
            out.writeInt(bytes.length); out.write(bytes);
            out.writeInt(table.length);
            for (Object o : table) writeValue(out, o, depth + 1);
        } finally {
            p.recycle();
        }
    }

    private static Object readValue(DataInputStream in, ClassLoader l, int depth) throws IOException {
        if (depth > DEPTH + 2) throw new IOException("nested too deep");
        int tag = in.readByte();
        switch (tag) {
        case NULL: return null;
        case STRING: return str(in);
        case INT: return in.readInt();
        case LONG: return in.readLong();
        case BOOLEAN: return in.readBoolean();
        case DOUBLE: return in.readDouble();
        case FLOAT: return in.readFloat();
        case SHORT: return in.readShort();
        case BYTE: return in.readByte();
        case CHAR: return in.readChar();
        case BYTES: return bytes(in);
        case INTS: { int[] a = new int[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readInt(); return a; }
        case LONGS: { long[] a = new long[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readLong(); return a; }
        case BOOLEANS: { boolean[] a = new boolean[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readBoolean(); return a; }
        case DOUBLES: { double[] a = new double[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readDouble(); return a; }
        case FLOATS: { float[] a = new float[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readFloat(); return a; }
        case SHORTS: { short[] a = new short[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readShort(); return a; }
        case CHARS: { char[] a = new char[count(in)]; for (int k = 0; k < a.length; k++) a[k] = in.readChar(); return a; }
        case STRINGS: { String[] a = new String[count(in)]; for (int k = 0; k < a.length; k++) a[k] = str(in); return a; }
        case TEXTS: { CharSequence[] a = new CharSequence[count(in)]; for (int k = 0; k < a.length; k++) a[k] = str(in); return a; }
        case BUNDLE: { Bundle b = new Bundle(); readBundle(in, b, l, depth + 1); return b; }
        case PERSISTABLE: { PersistableBundle b = new PersistableBundle(); readBundle(in, b, l, depth + 1); return b; }
        case INTENT: return readIntent(in, l, depth + 1);
        case COMPONENT: return ComponentName.unflattenFromString(str(in));
        case URI: return Uri.parse(str(in));
        case LIST: {
            ArrayList<Object> a = new ArrayList<>();
            for (int n = count(in); n > 0; n--) { Object x = readValue(in, l, depth + 1); if (x != DROPPED) a.add(x); }
            return a;
        }
        case MAP: {
            HashMap<Object, Object> m = new HashMap<>();
            for (int n = count(in); n > 0; n--) {
                Object k = readValue(in, l, depth + 1), x = readValue(in, l, depth + 1);
                if (k != DROPPED && x != DROPPED) m.put(k, x);
            }
            return m;
        }
        case PARCELABLES: {
            Parcelable[] a = new Parcelable[count(in)];
            for (int k = 0; k < a.length; k++) { Object x = readValue(in, l, depth + 1); a[k] = x instanceof Parcelable ? (Parcelable) x : null; }
            return a;
        }
        case OBJECTS: {
            Object[] a = new Object[count(in)];
            for (int k = 0; k < a.length; k++) { Object x = readValue(in, l, depth + 1); a[k] = x == DROPPED ? null : x; }
            return a;
        }
        case SPARSE: {
            android.util.SparseArray<Object> a = new android.util.SparseArray<>();
            for (int n = count(in); n > 0; n--) { int k = in.readInt(); Object x = readValue(in, l, depth + 1); if (x != DROPPED) a.put(k, x); }
            return a;
        }
        case SERIALIZABLE: {
            byte[] b = bytes(in);
            try (java.io.ObjectInputStream o = new java.io.ObjectInputStream(new ByteArrayInputStream(b)) {
                @Override protected Class<?> resolveClass(java.io.ObjectStreamClass d) throws IOException, ClassNotFoundException {
                    try { return Class.forName(d.getName(), false, l); } catch (ClassNotFoundException e) { return super.resolveClass(d); }
                }
            }) {
                return o.readObject();
            } catch (Exception | LinkageError e) {
                Log.w(TAG, "an intent from another app: a Serializable of it cannot be read here (" + e + ")");
                return DROPPED;
            }
        }
        case PARCELABLE: {
            String cls = str(in);
            byte[] b = bytes(in);
            Object[] table = new Object[count(in)];
            for (int k = 0; k < table.length; k++) { Object x = readValue(in, l, depth + 1); table[k] = x == DROPPED ? null : x; }
            return create(cls, b, table, l);
        }
        default: throw new IOException("an unknown value, " + tag);
        }
    }

    /* The Parcelable of the reader's class `cls`, read by its CREATOR from the bytes and the table its writer gave. */
    private static Object create(String cls, byte[] bytes, Object[] table, ClassLoader l) {
        Parcel p = Parcel.obtain();
        try {
            Object creator = Class.forName(cls, true, l).getField("CREATOR").get(null);
            if (!(creator instanceof Parcelable.Creator)) throw new IllegalStateException("it has no CREATOR");
            p.meglerRestore(bytes, table);
            return creator instanceof Parcelable.ClassLoaderCreator ? ((Parcelable.ClassLoaderCreator<?>) creator).createFromParcel(p, l)
                : ((Parcelable.Creator<?>) creator).createFromParcel(p);
        } catch (Throwable t) {
            // (as on Android, where the receiver's unparcelling of a class it lacks fails)
            Log.w(TAG, "an intent from another app: its " + cls + " cannot be read here (" + t + ") and is left out");
            return DROPPED;
        } finally {
            p.recycle();
        }
    }

    // ---- strings and byte arrays: a length, -1 for none, and the bytes

    private static void str(DataOutputStream out, String s) throws IOException {
        if (s == null) { out.writeInt(-1); return; }
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        out.writeInt(b.length);
        out.write(b);
    }

    private static String str(DataInputStream in) throws IOException {
        int n = in.readInt();
        if (n < 0) return null;
        if (n > in.available()) throw new IOException("a string longer than what is left");
        byte[] b = new byte[n];
        in.readFully(b);
        return new String(b, StandardCharsets.UTF_8);
    }

    private static byte[] bytes(DataInputStream in) throws IOException {
        byte[] b = new byte[count(in)];
        in.readFully(b);
        return b;
    }

    /* A count of things that follow, each at least a byte: never more than the bytes left. */
    private static int count(DataInputStream in) throws IOException {
        int n = in.readInt();
        if (n < 0 || n > in.available()) throw new IOException("a count of " + n + " with " + in.available() + " bytes left");
        return n;
    }
}
