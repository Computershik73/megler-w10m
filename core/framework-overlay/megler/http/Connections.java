package megler.http;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.UnknownHostException;
import java.security.GeneralSecurityException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.TimeUnit;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * An engine's HTTP/2 connections, one or more for each host and port, shared by its streams (Chromium's sessions):
 * a stream takes a connection that can take it, or the first thread that needs one makes it while the others wait for it.
 * A connection is TLS by a context of the platform's own conscrypt (never the process's default one, which an app or
 * Google Play services may have replaced), trusting what the app's network security configuration and Android's authorities
 * make trusted, as HttpsURLConnection does; the host's name checked as Android's HTTP client checks it, and ALPN offering
 * "h2" alone. A connection with no stream for five minutes is let go; one the server has been quiet on for 30 s is
 * asked for a PING when it is taken again, and closed if no answer comes in 10 s (a phone suspended, a network changed: its
 * streams then fail as a closed connection's, and the next ones make a new one).
 */
final class Connections {
    private static final int CONNECT_TIMEOUT_MS = 30000, HANDSHAKE_TIMEOUT_MS = 30000;
    private static final long IDLE_NS = TimeUnit.MINUTES.toNanos(5), QUIET_NS = TimeUnit.SECONDS.toNanos(30);
    private static final long PING_WAIT_MS = 10000;
    private static int said;

    private final Engine engine;
    private final HashMap<String, ArrayList<H2.Connection>> live = new HashMap<>();
    private final HashSet<String> connecting = new HashSet<>();
    private boolean shut;

    Connections(Engine engine) {
        this.engine = engine;
    }

    /** A stream on a connection to host:port, its HEADERS sent. */
    H2.Stream open(String host, int port, List<String[]> fields, boolean endStream) throws IOException {
        String key = host + ":" + port;
        for (int attempt = 0; ; attempt++) {
            boolean[] made = { false };
            H2.Connection c = acquire(key, host, port, made);
            try {
                return c.newStream(fields, endStream);
            } catch (H2.Unusable e) {
                if (attempt >= 2) throw e;
            } catch (H2.Ended e) {
                /* A connection taken again broke as the HEADERS went out: nothing reached the server. */
                if (made[0] || attempt >= 2) throw e;
            }
        }
    }

    private H2.Connection acquire(String key, String host, int port, boolean[] made) throws IOException {
        synchronized (this) {
            while (true) {
                if (shut) throw new IOException("the engine is shut down");
                H2.Connection c = usableLocked(key);
                if (c != null) return c;
                if (connecting.add(key)) break;
                try {
                    wait();
                } catch (InterruptedException e) {
                    throw new InterruptedIOException("interrupted while another thread connects to " + key);
                }
            }
        }
        H2.Connection c = null;
        try {
            c = connect(host, port);
        } finally {
            synchronized (this) {
                connecting.remove(key);
                if (c != null && !shut) {
                    ArrayList<H2.Connection> list = live.get(key);
                    if (list == null) live.put(key, list = new ArrayList<>());
                    list.add(c);
                }
                notifyAll();
            }
        }
        if (c.isClosed() || shut) {
            c.shutdown();
            throw new IOException("the engine is shut down");
        }
        made[0] = true;
        return c;
    }

    private H2.Connection usableLocked(String key) {
        ArrayList<H2.Connection> list = live.get(key);
        if (list == null) return null;
        H2.Connection best = null;
        for (Iterator<H2.Connection> i = list.iterator(); i.hasNext(); ) {
            H2.Connection c = i.next();
            if (c.isClosed()) {
                i.remove();
            } else if (c.idleNs() > IDLE_NS) {
                i.remove();
                c.shutdown();
            } else if (best == null && c.canTakeStream()) {
                best = c;
            }
        }
        if (list.isEmpty()) live.remove(key);
        if (best != null && best.quietNs() > QUIET_NS) checkAlive(best);
        return best;
    }

    private void checkAlive(final H2.Connection c) {
        if (!c.ping()) return;
        engine.timer().schedule(() -> {
            if (!c.pingAnswered()) c.close(new H2.Ended("no answer from " + c.host + " to a PING in " + PING_WAIT_MS / 1000 + " s", -1, false, null));
        }, PING_WAIT_MS, TimeUnit.MILLISECONDS);
    }

    /** The engine's shutdown: every connection closed. */
    void shutdown() {
        ArrayList<H2.Connection> all = new ArrayList<>();
        synchronized (this) {
            shut = true;
            for (ArrayList<H2.Connection> list : live.values()) all.addAll(list);
            live.clear();
            notifyAll();
        }
        for (H2.Connection c : all) c.shutdown();
    }

    /** TCP to the host's addresses in turn, TLS over it with ALPN "h2", the host's name checked, the HTTP/2 preface. */
    private H2.Connection connect(String host, int port) throws IOException {
        long began = System.nanoTime();
        String bare = host.startsWith("[") && host.endsWith("]") ? host.substring(1, host.length() - 1) : host;
        InetAddress[] addresses = InetAddress.getAllByName(bare);
        Socket plain = null;
        IOException last = null;
        for (InetAddress a : addresses) {
            Socket s = new Socket();
            try {
                s.connect(new InetSocketAddress(a, port), CONNECT_TIMEOUT_MS);
                plain = s;
                break;
            } catch (IOException e) {
                last = e;
                try {
                    s.close();
                } catch (IOException ignored) {
                }
            }
        }
        if (plain == null) throw last != null ? last : new UnknownHostException(host);
        SSLSocket ssl = null;
        try {
            plain.setTcpNoDelay(true);
            ssl = (SSLSocket) Tls.factory().createSocket(plain, bare, port, true);
            SSLParameters p = ssl.getSSLParameters();
            Tls.offer(p, "h2");
            ssl.setSSLParameters(p);
            ssl.setSoTimeout(HANDSHAKE_TIMEOUT_MS);
            ssl.startHandshake();
            if (!Tls.verifier().verify(bare, ssl.getSession())) throw new SSLPeerUnverifiedException("the certificate of " + host + " is not for " + host);
            String alpn = Tls.chosen(ssl);
            if (!"h2".equals(alpn)) throw new Failures.NoHttp2(host, alpn);
            ssl.setSoTimeout(0);
            H2.Connection c = new H2.Connection(host, port, ssl);
            c.start();
            say("HttpEngine: HTTP/2 to " + host + ":" + port + " (" + ssl.getSession().getProtocol() + ", " + ssl.getSession().getCipherSuite() + ") in "
                    + (System.nanoTime() - began) / 1000000 + " ms");
            return c;
        } catch (IOException | RuntimeException e) {
            try {
                if (ssl != null) ssl.close();
                else plain.close();
            } catch (IOException ignored) {
            }
            throw e;
        }
    }

    /** At most sixteen lines a process: which connections an app's HttpEngine made. */
    private static void say(String line) {
        synchronized (Connections.class) {
            if (said >= 16) return;
            said++;
        }
        android.util.Log.i("Megler", line);
    }

    /** The platform's TLS, as Android's HTTP client has it, whatever defaults the app has set for HttpsURLConnection or
     *  for SSLContext. */
    static final class Tls {
        private static HostnameVerifier verifier;
        private static SSLSocketFactory factory;

        /**
         * Sockets of the platform's conscrypt ("AndroidOpenSSL") with the platform's trust, as Chromium's engine has a TLS of
         * its own: not SSLContext.getDefault(), which is the process's to set. Google Play services' ProviderInstaller makes
         * the Conscrypt it carries the default provider and the default context, and that one's sockets take no ALPN
         * protocols from SSLParameters (its Platform.setSSLParameters reads the host name, the endpoint identification and
         * the cipher order): through it "h2" was never offered, and every stream of the Play Store's ended "did not take
         * HTTP/2 (ALPN none)" (a Lumia 950, 0.1.0.690, from 2.5 s after the provider was installed).
         */
        static synchronized SSLSocketFactory factory() throws IOException {
            if (factory == null) {
                try {
                    SSLContext context = SSLContext.getInstance("TLS", "AndroidOpenSSL");
                    context.init(null, null, null);
                    factory = context.getSocketFactory();
                } catch (GeneralSecurityException e) {
                    throw new SSLException("no TLS here: " + e.getMessage(), e);
                }
            }
            return factory;
        }

        /** Android's own host name check (okhttp's OkHostnameVerifier); HttpsURLConnection's default where it is not there. */
        static synchronized HostnameVerifier verifier() {
            if (verifier == null) {
                try {
                    verifier = (HostnameVerifier) Class.forName("com.android.okhttp.internal.tls.OkHostnameVerifier").getField("INSTANCE").get(null);
                } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
                    verifier = HttpsURLConnection.getDefaultHostnameVerifier();
                }
            }
            return verifier;
        }

        /** ALPN: SSLParameters.setApplicationProtocols (Android 10's, which Megler's libcore has; JDK 8 compiles without it). */
        static void offer(SSLParameters p, String... protocols) throws SSLException {
            try {
                SSLParameters.class.getMethod("setApplicationProtocols", String[].class).invoke(p, (Object) protocols);
            } catch (ReflectiveOperationException e) {
                throw new SSLException("no ALPN here: " + e, e);
            }
        }

        /** The protocol the server chose by ALPN, null or "" for none. */
        static String chosen(SSLSocket s) {
            try {
                return (String) SSLSocket.class.getMethod("getApplicationProtocol").invoke(s);
            } catch (ReflectiveOperationException | RuntimeException e) {
                try {
                    byte[] b = (byte[]) s.getClass().getMethod("getAlpnSelectedProtocol").invoke(s);
                    return b == null ? null : new String(b, java.nio.charset.StandardCharsets.US_ASCII);
                } catch (ReflectiveOperationException | RuntimeException e2) {
                    return null;
                }
            }
        }

        /** An https connection of Android's HTTP client given the platform's TLS (the app may have changed the defaults). */
        static void platform(HttpsURLConnection c) throws IOException {
            c.setSSLSocketFactory(factory());
            c.setHostnameVerifier(verifier());
        }
    }
}
