package megler;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/*
 * Megler: the flag cronet-netlog. Cronet's own network log (its NetLog, what chrome://net-export writes) of an app that
 * carries Cronet in its own code, as YouTube does (libcronet.so and org.chromium.net.impl): what goes out and comes back
 * is otherwise sealed inside the library's own TLS, and a server's refusal ("Request contains an invalid argument", a
 * 400 on YouTube's channel pages) says nothing of the request it refused. Each CronetUrlRequestContext the app makes
 * starts logging as it is made, with everything (logAll: the bytes of every request and response, cookies and
 * credentials included -- the log stays in the app's data, for the diagnosis alone), into files/netlog-<n>/ of the app's
 * data, at most 64 MB for each (Cronet keeps the newest). Cronet writes it as it goes, in netlog.json.inprogress/; the
 * app ending without stopNetLog leaves it there unstitched. TLS's bytes only: QUIC's are not in it, so the flag no-quic
 * goes with it (core/src/bionic/mg_net.c).
 *
 * And the bodies the app hands Cronet whole (UploadDataProviders.create, of a byte[] or a ByteBuffer: YouTube's InnerTube
 * requests), each as it is, into files/netlog-bodies/<n>.bin, with files/netlog-bodies/index.txt saying the URL the same
 * thread last made a request of (CronetEngine.newUrlRequestBuilder), its time and its length: at most 4000 bodies and 48 MB.
 * Nothing of the kind without the flag.
 */
final class CronetNetLog {
    private static final String CONTEXT = "org.chromium.net.impl.CronetUrlRequestContext";
    private static final int MOST = 64 << 20;
    private static final int MOST_BODIES = 4000;
    private static final long MOST_BODY_BYTES = 48L << 20;
    private static final int KIND_CONTEXT = 0, KIND_BUILDER = 1, KIND_BODY = 2;
    private static final ThreadLocal<String> sUrl = new ThreadLocal<>();
    private static int sMade, sBodies;
    private static long sBodyBytes;

    private final int mKind;
    private Method mOriginal;

    private CronetNetLog(int kind) { mKind = kind; }

    /** With the flag: the app's Cronet hooked, through `loader` (the app's), before its code runs. */
    static void install(ClassLoader loader) {
        if (!Native.flag("cronet-netlog")) return;
        Class<?> c;
        try {
            c = Class.forName(CONTEXT, false, loader);
        } catch (ClassNotFoundException e) {
            android.util.Log.i("Megler", "cronet-netlog: the app has no Cronet of its own (" + CONTEXT + ")");
            return;
        }
        try {
            for (Constructor<?> k : c.getDeclaredConstructors()) hook(KIND_CONTEXT, k);
            // (each body of newUrlRequestBuilder(String, Callback, Executor) the class has or inherits: a covariant one's bridge too)
            for (Class<?> k = c; k != null && k != Object.class; k = k.getSuperclass())
                for (Method m : k.getDeclaredMethods())
                    if (m.getName().equals("newUrlRequestBuilder") && m.getParameterTypes().length == 3 && m.getParameterTypes()[0] == String.class
                        && !java.lang.reflect.Modifier.isAbstract(m.getModifiers())) hook(KIND_BUILDER, m);
            Class<?> providers = Class.forName("org.chromium.net.UploadDataProviders", false, loader);
            hook(KIND_BODY, providers.getMethod("create", byte[].class));
            hook(KIND_BODY, providers.getMethod("create", ByteBuffer.class));
        } catch (Throwable t) {
            android.util.Log.w("Megler", "cronet-netlog: Cronet not hooked whole", t);
        }
    }

    private static void hook(int kind, Member m) throws ReflectiveOperationException {
        CronetNetLog h = new CronetNetLog(kind);
        h.mOriginal = Hooks.hook0(h, m, CronetNetLog.class.getDeclaredMethod("callback", Object[].class));
        if (h.mOriginal != null) h.mOriginal.setAccessible(true);
        android.util.Log.i("Megler", "cronet-netlog: " + m + (h.mOriginal != null ? " hooked" : " not hooked"));
    }

    /** A call of a hooked method: the original, and what the flag wants of it. */
    Object callback(Object[] args) {
        boolean statik = mKind == KIND_BODY;
        Object self = statik ? null : args[0];
        Object[] rest = new Object[statik ? args.length : args.length - 1];
        System.arraycopy(args, statik ? 0 : 1, rest, 0, rest.length);
        if (mKind == KIND_BUILDER && rest[0] instanceof String) sUrl.set((String) rest[0]);
        if (mKind == KIND_BODY) body(rest[0]);
        Object r;
        try {
            r = mOriginal.invoke(self, rest);
        } catch (InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof RuntimeException) throw (RuntimeException) t;
            if (t instanceof Error) throw (Error) t;
            throw new RuntimeException(t);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
        if (mKind == KIND_CONTEXT) start(self);
        return r;
    }

    private static void start(Object context) {
        int n;
        synchronized (CronetNetLog.class) { n = sMade++; }
        File dir = new File(ContextImpl.app().getFilesDir(), "netlog-" + n);
        try {
            dir.mkdirs();
            // Cronet asks whether <dir>/netlog.json is writable (base::PathIsWritable: access(W_OK)) before it starts, and a
            // file that is not there yet is not ("Path is not writable"): it is made empty first. Cronet writes its log into
            // netlog.json.inprogress/ beside it and stitches it into this file when the log is stopped.
            new File(dir, "netlog.json").createNewFile();
            context.getClass().getMethod("startNetLogToDisk", String.class, boolean.class, int.class).invoke(context, dir.getPath(), true, MOST);
            android.util.Log.i("Megler", "cronet-netlog: Cronet's network log of " + context.getClass().getName() + "@"
                + Integer.toHexString(System.identityHashCode(context)) + " goes to " + dir + " (everything, at most " + (MOST >> 20) + " MB)");
        } catch (Throwable t) {
            android.util.Log.w("Megler", "cronet-netlog: the log of " + context + " did not start", t);
        }
    }

    /** A body the app hands Cronet whole, kept as it is (a ByteBuffer's remaining bytes, its position left alone). */
    private static void body(Object data) {
        byte[] b;
        if (data instanceof byte[]) {
            b = (byte[]) data;
        } else if (data instanceof ByteBuffer) {
            ByteBuffer d = ((ByteBuffer) data).duplicate();
            b = new byte[d.remaining()];
            d.get(b);
        } else {
            return;
        }
        int n;
        synchronized (CronetNetLog.class) {
            if (sBodies >= MOST_BODIES || sBodyBytes + b.length > MOST_BODY_BYTES) return;
            n = sBodies++;
            sBodyBytes += b.length;
        }
        File dir = new File(ContextImpl.app().getFilesDir(), "netlog-bodies");
        try {
            dir.mkdirs();
            try (FileOutputStream out = new FileOutputStream(new File(dir, n + ".bin"))) { out.write(b); }
            String line = n + "\t" + System.currentTimeMillis() + "\t" + Thread.currentThread().getName() + "\t" + b.length + "\t" + sUrl.get() + "\n";
            synchronized (CronetNetLog.class) {
                try (FileOutputStream out = new FileOutputStream(new File(dir, "index.txt"), true)) { out.write(line.getBytes("UTF-8")); }
            }
        } catch (Throwable t) {
            android.util.Log.w("Megler", "cronet-netlog: body " + n + " not kept", t);
        }
    }
}
