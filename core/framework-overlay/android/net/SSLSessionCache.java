package android.net;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import javax.net.ssl.SSLContext;

/** Persistent TLS sessions in the application's own directory, through Conscrypt. */
public final class SSLSessionCache {
    final Object sessionCache;
    public SSLSessionCache(Object cache) {
        try { sessionCache = Class.forName("com.android.org.conscrypt.SSLClientSessionCache").cast(cache); }
        catch (ClassNotFoundException e) { throw new IllegalStateException(e); }
    }
    public SSLSessionCache(File dir) throws IOException { sessionCache = open(dir); }
    public SSLSessionCache(Context context) {
        Object cache = null;
        try { cache = open(context.getDir("sslcache", Context.MODE_PRIVATE)); }
        catch (IOException e) { android.util.Log.w("SSLSessionCache", "Cannot open session cache", e); }
        sessionCache = cache;
    }
    private static Object open(File dir) throws IOException {
        return SSLCertificateSocketFactory.call("com.android.org.conscrypt.FileClientSessionCache", null,
                "usingDirectory", new Class<?>[]{File.class}, dir);
    }
    public static void install(SSLSessionCache cache, SSLContext context) {
        try {
            Class<?> type = Class.forName("com.android.org.conscrypt.ClientSessionContext");
            Object client = context.getClientSessionContext();
            if (!type.isInstance(client)) throw new IllegalArgumentException("Incompatible SSLContext: " + context);
            SSLCertificateSocketFactory.call(type.getName(), client, "setPersistentCache",
                    new Class<?>[]{Class.forName("com.android.org.conscrypt.SSLClientSessionCache")},
                    cache == null ? null : cache.sessionCache);
        } catch (ClassNotFoundException | IOException e) { throw new IllegalStateException(e); }
    }
}
