package capture;

import de.robv.android.xposed.XposedBridge;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import android.util.Log;

/** Pass-through capture of one GMS version's protocol; neither requests nor VM results are replaced. */
public final class Probe {
    private static final AtomicInteger sequence = new AtomicInteger();
    private static File directory;
    public static void install(ClassLoader gms, File out) throws Exception {
        directory = out;
        if (!directory.mkdirs() && !directory.isDirectory()) throw new IOException("capture directory");
        hook(gms, "byvy", "a", "request", "hzgv", "cudk");
        hook(gms, "byuc", "a", "signed", "[B", "[B", "java.lang.String");
        hook(gms, "byun", "c", "vm", "java.util.Map");
        hook(gms, "byun", "b", "init");
        Log.i("DGCapture", "Network, signature and VM-result capture installed");
    }
    private static void hook(ClassLoader loader, String cls, String name, String kind, String... types) throws Exception {
        Class<?>[] parameters = new Class<?>[types.length];
        for (int i = 0; i < types.length; i++) parameters[i] = Class.forName(types[i], false, loader);
        Method original = Class.forName(cls, false, loader).getDeclaredMethod(name, parameters);
        Capture hook = new Capture(kind);
        hook.backup = XposedBridge.hook0(hook, original, Capture.class.getMethod("callback", Object[].class));
        if (hook.backup == null) throw new IllegalStateException("hook failed: " + cls);
    }
    private static String flow(Object request) throws Exception {
        Object config = request.getClass().getField("c").get(request);
        return config == null ? "none" : (String) config.getClass().getField("c").get(config);
    }
    private static byte[] serialize(Object message) throws Exception {
        Method selected = null;
        for (Method method : message.getClass().getMethods()) {
            if (method.getReturnType() == byte[].class && method.getParameterTypes().length == 0) {
                if (selected != null) throw new IllegalStateException("ambiguous serializer");
                selected = method;
            }
        }
        if (selected == null) throw new NoSuchMethodException("protobuf byte[] serializer");
        return (byte[]) selected.invoke(message);
    }
    private static void save(String name, byte[] bytes) throws Exception {
        if (bytes == null || bytes.length > (2 << 20)) throw new IOException("capture size");
        File file = new File(directory, name.replaceAll("[^A-Za-z0-9_.-]", "_") + ".bin");
        try (FileOutputStream out = new FileOutputStream(file)) { out.write(bytes); }
        Log.i("DGCapture", file.getName() + " bytes=" + bytes.length);
    }
    public static final class Capture {
        final String kind;
        Method backup;
        Capture(String kind) { this.kind = kind; }
        private Object original(Object[] args) throws Throwable {
            try { return backup.invoke(args[0], Arrays.copyOfRange(args, 1, args.length)); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        public Object callback(Object[] args) throws Throwable {
            int order = sequence.incrementAndGet();
            if (order > 128) return original(args);
            String label = String.format(java.util.Locale.ROOT, "%03d-t%d-%s", order, Thread.currentThread().getId(), kind);
            try {
                if (kind.equals("request")) {
                    label += "-" + flow(args[1]);
                    save(label, serialize(args[1]));
                } else if (kind.equals("signed")) {
                    label += "-" + args[3];
                    save(label + "-body", (byte[]) args[1]);
                    save(label + "-signature", (byte[]) args[2]);
                }
            } catch (Throwable error) { Log.e("DGCapture", "capture before call failed", error); }
            Object value;
            long began = android.os.SystemClock.elapsedRealtime();
            Log.i("DGCapture", label + " start=" + began);
            try { value = original(args); }
            finally { Log.i("DGCapture", label + " end=" + android.os.SystemClock.elapsedRealtime()
                    + " duration=" + (android.os.SystemClock.elapsedRealtime() - began)); }
            try {
                if (kind.equals("vm")) save(label + "-result", (byte[]) value);
                if (kind.equals("signed")) Log.i("DGCapture", label + " verified=" + value);
            } catch (Throwable error) { Log.e("DGCapture", "capture after call failed", error); }
            return value;
        }
    }
}
