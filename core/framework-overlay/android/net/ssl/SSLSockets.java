package android.net.ssl;

import java.lang.reflect.InvocationTargetException;
import javax.net.ssl.SSLSocket;

/** Android 10 socket helpers backed by the platform Conscrypt provider.
 * Reflection keeps the framework build independent of the boot DEX files. */
public final class SSLSockets {
    private SSLSockets() {}

    public static boolean isSupportedSocket(SSLSocket socket) {
        try {
            return (Boolean) Class.forName("com.android.org.conscrypt.Conscrypt")
                    .getMethod("isConscrypt", SSLSocket.class).invoke(null, socket);
        } catch (ClassNotFoundException e) {
            return false;
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Conscrypt socket API unavailable", e);
        }
    }

    public static void setUseSessionTickets(SSLSocket socket, boolean enabled) {
        if (!isSupportedSocket(socket)) throw new IllegalArgumentException("Unsupported socket");
        try {
            Class.forName("com.android.org.conscrypt.Conscrypt")
                    .getMethod("setUseSessionTickets", SSLSocket.class, boolean.class)
                    .invoke(null, socket, enabled);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw new IllegalStateException(cause);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Conscrypt session tickets unavailable", e);
        }
    }
}
