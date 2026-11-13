// Overlay of core/framework/src/android/content/MeglerSharedPreferences.java (tools/build-framework.sh).
// It differs in how the file is written: into a file beside it (the name with ".new" after it), synced, then renamed
// over it, as Android's SharedPreferencesImpl keeps a backup so that a write cut short leaves the old file. The original
// wrote the file itself: opening it emptied it and the lines went in only when it was closed, so an app that ended in
// between -- a crash, the phone restarting, an update of the package -- found its preferences empty the next time
// (Asphalt 9's A9HM.xml was found 0 bytes after the game had ended in a race).
// A ".new" file left by a write cut short is deleted when the preferences are read.
//
// And preferences in the services' shared folder (/data/megler-services: microG's, which every app that hosts microG and
// microG's own app use at once) are kept for several processes. microG keeps its settings in SharedPreferences behind its
// own provider, counting on one process for them; here each process has the provider and a copy of the file in memory, and
// wrote its whole copy on every change: a process that had read the file before the user turned device registration on in
// microG's settings turned it off again with its next write of any other setting (seen on the phone). Such a file is read
// again when another process has written it since (its time and length), and a write puts this process's changes alone
// on what the file holds then.
//
// And a null key is kept, as Android keeps it: writing the file ended in a NullPointerException with one in the map, at every
// write from then on, so nothing the app saved afterwards reached the file (Lemana PRO: the InAppStory SDK puts a value under
// a null key). Everything else is the original's.
package android.content;

import java.io.*;
import java.util.*;

/** SharedPreferences kept in a file of typed lines ("i key value"), written on every commit. */
public final class MeglerSharedPreferences implements SharedPreferences {
    private final File file;
    private final HashMap<String, Object> map = new HashMap<>();
    private final java.util.concurrent.CopyOnWriteArrayList<OnSharedPreferenceChangeListener> listeners = new java.util.concurrent.CopyOnWriteArrayList<>();
    private static final android.os.Handler MAIN = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final java.util.concurrent.ExecutorService WRITER = java.util.concurrent.Executors.newSingleThreadExecutor(r -> { Thread t = new Thread(r, "SharedPreferencesWriter"); t.setDaemon(true); return t; });
    private final java.util.concurrent.atomic.AtomicBoolean writePending = new java.util.concurrent.atomic.AtomicBoolean();
    /** A file several processes use (see the note at the top), the file's time and length as this process last saw them, and the
        changes this process made that are not written yet. */
    private final boolean shared;
    private long seenTime = -1, seenLength = -1;
    private final HashMap<String, Object> unwrittenPuts = new HashMap<>();
    private final HashSet<String> unwrittenRemoves = new HashSet<>();
    private boolean unwrittenClear;

    public MeglerSharedPreferences(File f) { file = f; shared = sharedFile(f); load(); }

    /** Whether the file is in the services' shared folder, as an app hosting microG or microG's own app (whose files are there) names it. */
    private static boolean sharedFile(File f) {
        String path = f.getAbsolutePath();
        if (path.startsWith("/data/megler-services/")) return true;
        String gms = "com.google.android.gms";
        return (path.startsWith("/data/data/" + gms + "/") || path.startsWith("/data/user/0/" + gms + "/")) && gms.equals(megler.Native.packageName());
    }
    private void see() { seenTime = file.lastModified(); seenLength = file.length(); }
    /** A shared file another process wrote since: read again, with this process's unwritten changes on top. */
    private synchronized void refresh() {
        if (!shared || (file.lastModified() == seenTime && file.length() == seenLength)) return;
        map.clear();
        read(map);
        see();
        overlayUnwritten(map);
    }
    private void overlayUnwritten(HashMap<String, Object> into) {
        if (unwrittenClear) into.clear();
        for (String k : unwrittenRemoves) into.remove(k);
        into.putAll(unwrittenPuts);
    }

    private static String esc(String s) { return s.replace("\\", "\\\\").replace("\n", "\\n"); }
    private static String unesc(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); if (c == '\\' && i + 1 < s.length()) { char n = s.charAt(++i); b.append(n == 'n' ? '\n' : n); } else b.append(c); }
        return b.toString();
    }
    /* A null key, which Android keeps (its map holds it and the XML has an entry with no name): a backslash and a zero,
       which no written key is -- esc doubles a key's own backslashes. */
    private static final String NULL_KEY = "\\0";
    private File written() { return new File(file.getPath() + ".new"); }
    private void load() {
        // (Not in a shared file: there it may be another process's write in progress.)
        if (!shared) written().delete();
        read(map);
        see();
    }
    /* The space after a line's key: one the writer escaped ("\ ") is the key's own. GmsCore keys some by an account
       ("Account {name=..., type=com.google}"), and the number of such a line was read from the key's middle. */
    private static int keyEnd(String line) {
        for (int i = 2; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '\\') i++;
            else if (c == ' ') return i;
        }
        return -1;
    }
    private void read(HashMap<String, Object> map) {
        if (!file.exists()) return;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file), "UTF-8"))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.length() < 3) continue;
                char t = line.charAt(0);
                int sp = keyEnd(line);
                if (sp < 0) continue;
                String raw = line.substring(2, sp), k = NULL_KEY.equals(raw) ? null : unesc(raw), v = line.substring(sp + 1);
                try {
                    read(map, t, k, v);
                } catch (NumberFormatException e) {
                    /* A line that is not what its type says is left out, not the app ended. */
                    android.util.Log.w("Megler", "preferences " + file.getName() + ": a value of type " + t + " that is not one, left out");
                }
            }
        } catch (IOException e) { }
    }
    private static void read(HashMap<String, Object> map, char t, String k, String v) {
        switch (t) {
        case 's': map.put(k, unesc(v)); break;
        case 'i': map.put(k, Integer.parseInt(v)); break;
        case 'l': map.put(k, Long.parseLong(v)); break;
        case 'f': map.put(k, Float.parseFloat(v)); break;
        case 'b': map.put(k, Boolean.parseBoolean(v)); break;
        case 'S': { HashSet<String> set = new HashSet<>(); for (String p : v.split("\u0001", -1)) if (!p.isEmpty()) set.add(unesc(p)); map.put(k, set); break; }
        }
    }
    private synchronized boolean save() {
        file.getParentFile().mkdirs();
        if (shared) {
            // What the file holds now, this process's changes on it: another process's settings stay as it wrote them.
            HashMap<String, Object> now = new HashMap<>();
            read(now);
            overlayUnwritten(now);
            map.clear();
            map.putAll(now);
        }
        File next = shared ? new File(file.getPath() + "." + android.os.Process.myPid() + ".new") : written();
        if (!write(next) || !next.renameTo(file)) { next.delete(); return false; }
        unwrittenPuts.clear();
        unwrittenRemoves.clear();
        unwrittenClear = false;
        see();
        return true;
    }
    private boolean write(File to) {
        try (FileOutputStream out = new FileOutputStream(to); Writer w = new OutputStreamWriter(out, "UTF-8")) {
            for (Map.Entry<String, Object> e : map.entrySet()) {
                Object v = e.getValue(); String k = e.getKey() == null ? NULL_KEY : esc(e.getKey()).replace(" ", "\\ ");
                if (v instanceof String) w.write("s " + k + " " + esc((String) v) + "\n");
                else if (v instanceof Integer) w.write("i " + k + " " + v + "\n");
                else if (v instanceof Long) w.write("l " + k + " " + v + "\n");
                else if (v instanceof Float) w.write("f " + k + " " + v + "\n");
                else if (v instanceof Boolean) w.write("b " + k + " " + v + "\n");
                else if (v instanceof Set) { StringBuilder b = new StringBuilder(); for (Object o : (Set<?>) v) b.append(esc(String.valueOf(o))).append('\u0001'); w.write("S " + k + " " + b + "\n"); }
            }
            w.flush();
            out.getFD().sync();
            return true;
        } catch (IOException e) { return false; }
    }
    public synchronized Map<String, ?> getAll() { refresh(); return new HashMap<>(map); }
    public synchronized String getString(String k, String d) { refresh(); Object v = map.get(k); return v instanceof String ? (String) v : d; }
    @SuppressWarnings("unchecked") public synchronized Set<String> getStringSet(String k, Set<String> d) { refresh(); Object v = map.get(k); return v instanceof Set ? (Set<String>) v : d; }
    public synchronized int getInt(String k, int d) { refresh(); Object v = map.get(k); return v instanceof Integer ? (Integer) v : d; }
    public synchronized long getLong(String k, long d) { refresh(); Object v = map.get(k); return v instanceof Long ? (Long) v : v instanceof Integer ? (Integer) v : d; }
    public synchronized float getFloat(String k, float d) { refresh(); Object v = map.get(k); return v instanceof Float ? (Float) v : d; }
    public synchronized boolean getBoolean(String k, boolean d) { refresh(); Object v = map.get(k); return v instanceof Boolean ? (Boolean) v : d; }
    public synchronized boolean contains(String k) { refresh(); return map.containsKey(k); }
    public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { if (l != null && !listeners.contains(l)) listeners.add(l); }
    public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { listeners.remove(l); }
    /* As Android's: an editor's changes go to memory at once (under the preferences' lock), apply() writes the file in the
       background (one write for a burst), listeners hear each changed key on the main thread. Editors may be shared between threads. */
    private void notifyChanged(List<String> keys) {
        if (listeners.isEmpty() || keys.isEmpty()) return;
        Runnable r = () -> { for (OnSharedPreferenceChangeListener l : listeners) for (String k : keys) l.onSharedPreferenceChanged(this, k); };
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) r.run(); else MAIN.post(r);
    }
    private List<String> toMemory(boolean clear, Set<String> removes, Map<String, Object> puts) {
        ArrayList<String> changed = new ArrayList<>();
        synchronized (this) {
            refresh();
            if (clear && !map.isEmpty()) map.clear();
            for (String k : removes) if (map.remove(k) != null) changed.add(k);
            for (Map.Entry<String, Object> e : puts.entrySet()) {
                Object old = e.getValue() == null ? map.remove(e.getKey()) : map.put(e.getKey(), e.getValue());
                if (!Objects.equals(old, e.getValue())) changed.add(e.getKey());
            }
            if (shared) {
                if (clear) { unwrittenClear = true; unwrittenPuts.clear(); unwrittenRemoves.clear(); }
                for (String k : removes) { unwrittenPuts.remove(k); unwrittenRemoves.add(k); }
                for (Map.Entry<String, Object> e : puts.entrySet()) {
                    if (e.getValue() == null) { unwrittenPuts.remove(e.getKey()); unwrittenRemoves.add(e.getKey()); }
                    else { unwrittenRemoves.remove(e.getKey()); unwrittenPuts.put(e.getKey(), e.getValue()); }
                }
            }
        }
        return changed;
    }
    /** As QueuedWork.waitToFinish: apply()'s pending writes done before an activity pauses or stops (the app may be killed after). */
    public static void flushAll() { try { WRITER.submit(() -> {}).get(2, java.util.concurrent.TimeUnit.SECONDS); } catch (Exception e) {} }
    public Editor edit() {
        return new Editor() {
            private final HashMap<String, Object> puts = new HashMap<>();
            private final HashSet<String> removes = new HashSet<>();
            private boolean clear;
            public synchronized Editor putString(String k, String v) { puts.put(k, v); return this; }
            public synchronized Editor putStringSet(String k, Set<String> v) { puts.put(k, v == null ? null : new HashSet<>(v)); return this; }
            public synchronized Editor putInt(String k, int v) { puts.put(k, v); return this; }
            public synchronized Editor putLong(String k, long v) { puts.put(k, v); return this; }
            public synchronized Editor putFloat(String k, float v) { puts.put(k, v); return this; }
            public synchronized Editor putBoolean(String k, boolean v) { puts.put(k, v); return this; }
            public synchronized Editor remove(String k) { removes.add(k); return this; }
            public synchronized Editor clear() { clear = true; return this; }
            private List<String> takeAndApply() {
                boolean c; HashSet<String> r; HashMap<String, Object> p;
                synchronized (this) { c = clear; r = new HashSet<>(removes); p = new HashMap<>(puts); clear = false; removes.clear(); puts.clear(); }
                return toMemory(c, r, p);
            }
            public boolean commit() { List<String> ch = takeAndApply(); boolean ok = save(); notifyChanged(ch); return ok; }
            public void apply() {
                List<String> ch = takeAndApply();
                if (writePending.compareAndSet(false, true)) WRITER.execute(() -> { writePending.set(false); save(); });
                notifyChanged(ch);
            }
        };
    }
}
