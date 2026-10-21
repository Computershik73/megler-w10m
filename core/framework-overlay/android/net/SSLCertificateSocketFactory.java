package android.net;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.net.*;
import java.security.*;
import java.security.cert.X509Certificate;
import javax.net.SocketFactory;
import javax.net.ssl.*;

/** Android's legacy TLS factory, backed by the boot Conscrypt provider.
 * Reflection keeps the framework independent of the boot DEX at build time. */
public class SSLCertificateSocketFactory extends SSLSocketFactory {
    private final int handshakeTimeout;
    private final SSLSessionCache cache;
    private final boolean secure;
    private KeyManager[] keys;
    private TrustManager[] trust;
    private SSLSocketFactory delegate;
    private byte[] alpn, npn;
    private PrivateKey channelId;
    private static final TrustManager[] INSECURE = { new X509TrustManager() {
        public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
        public void checkClientTrusted(X509Certificate[] chain, String auth) {}
        public void checkServerTrusted(X509Certificate[] chain, String auth) {}
    }};

    public SSLCertificateSocketFactory(int timeout) { this(timeout, null, true); }
    private SSLCertificateSocketFactory(int timeout, SSLSessionCache cache, boolean secure) {
        this.handshakeTimeout = timeout; this.cache = cache; this.secure = secure;
    }
    public static SocketFactory getDefault(int timeout) { return new SSLCertificateSocketFactory(timeout); }
    public static SSLSocketFactory getDefault(int timeout, SSLSessionCache cache) {
        return new SSLCertificateSocketFactory(timeout, cache, true);
    }
    public static SSLSocketFactory getInsecure(int timeout, SSLSessionCache cache) {
        return new SSLCertificateSocketFactory(timeout, cache, false);
    }
    private synchronized SSLSocketFactory delegate() {
        if (delegate == null) {
            try {
                SSLContext context = SSLContext.getInstance("TLS", "AndroidOpenSSL");
                context.init(keys, secure ? trust : INSECURE, null);
                if (cache != null) SSLSessionCache.install(cache, context);
                delegate = context.getSocketFactory();
            } catch (GeneralSecurityException e) { throw new IllegalStateException("Cannot initialize Conscrypt TLS", e); }
        }
        return delegate;
    }
    public synchronized void setTrustManagers(TrustManager[] managers) { trust = managers == null ? null : managers.clone(); delegate = null; }
    public synchronized void setKeyManagers(KeyManager[] managers) { keys = managers == null ? null : managers.clone(); delegate = null; }
    public void setAlpnProtocols(byte[][] protocols) { alpn = toLengthPrefixedList(protocols); }
    public void setNpnProtocols(byte[][] protocols) { npn = toLengthPrefixedList(protocols); }
    public void setChannelIdPrivateKey(PrivateKey key) { channelId = key; }
    public static byte[] toLengthPrefixedList(byte[][] protocols) {
        if (protocols.length == 0) throw new IllegalArgumentException("protocols is empty");
        int size = 0;
        for (byte[] p : protocols) {
            if (p.length == 0 || p.length > 255) throw new IllegalArgumentException("protocol length must be 1..255");
            size += p.length + 1;
        }
        byte[] out = new byte[size]; int at = 0;
        for (byte[] p : protocols) { out[at++] = (byte)p.length; System.arraycopy(p, 0, out, at, p.length); at += p.length; }
        return out;
    }
    static Object call(String owner, Object target, String name, Class<?>[] types, Object... args) throws IOException {
        try { return Class.forName(owner).getMethod(name, types).invoke(target, args); }
        catch (InvocationTargetException e) {
            Throwable c = e.getCause();
            if (c instanceof IOException) throw (IOException)c;
            if (c instanceof RuntimeException) throw (RuntimeException)c;
            if (c instanceof Error) throw (Error)c;
            throw new IOException(c);
        } catch (ReflectiveOperationException e) { throw new IllegalStateException("Conscrypt API unavailable: " + name, e); }
    }
    private static Object socketCall(Socket socket, String name, Class<?>[] types, Object... args) throws IOException {
        try {
            Class<?> type = Class.forName("com.android.org.conscrypt.OpenSSLSocketImpl");
            if (!type.isInstance(socket)) throw new IllegalArgumentException("Not a Conscrypt socket: " + socket);
        } catch (ClassNotFoundException e) { throw new IllegalStateException(e); }
        return call("com.android.org.conscrypt.OpenSSLSocketImpl", socket, name, types, args);
    }
    private static Object setting(Socket s, String method, Class<?>[] types, Object... args) {
        try { return socketCall(s, method, types, args); }
        catch (IOException e) { throw new IllegalStateException(e); }
    }
    public void setHostname(Socket s, String host) { setting(s, "setHostname", new Class<?>[]{String.class}, host); }
    public void setUseSessionTickets(Socket s, boolean use) { setting(s, "setUseSessionTickets", new Class<?>[]{boolean.class}, use); }
    public void setSoWriteTimeout(Socket s, int timeout) throws SocketException {
        try { socketCall(s, "setSoWriteTimeout", new Class<?>[]{int.class}, timeout); }
        catch (SocketException e) { throw e; }
        catch (IOException e) { SocketException out = new SocketException(e.toString()); out.initCause(e); throw out; }
    }
    public byte[] getAlpnSelectedProtocol(Socket s) { return (byte[])setting(s, "getAlpnSelectedProtocol", new Class<?>[0]); }
    public byte[] getNpnSelectedProtocol(Socket s) { return (byte[])setting(s, "getNpnSelectedProtocol", new Class<?>[0]); }
    private Socket configure(Socket s, String host) throws IOException {
        try {
            socketCall(s, "setNpnProtocols", new Class<?>[]{byte[].class}, (Object)npn);
            socketCall(s, "setAlpnProtocols", new Class<?>[]{byte[].class}, (Object)alpn);
            socketCall(s, "setHandshakeTimeout", new Class<?>[]{int.class}, handshakeTimeout);
            socketCall(s, "setChannelIdPrivateKey", new Class<?>[]{PrivateKey.class}, channelId);
            if (secure && host != null) verifyHostname(s, host);
            return s;
        } catch (IOException | RuntimeException e) {
            try { s.close(); } catch (IOException ignored) {}
            throw e;
        }
    }
    public static void verifyHostname(Socket socket, String hostname) throws IOException {
        if (!(socket instanceof SSLSocket)) throw new IllegalArgumentException("Not an SSL socket");
        SSLSocket ssl = (SSLSocket)socket;
        ssl.startHandshake();
        SSLSession session = ssl.getSession();
        if (session == null || !HttpsURLConnection.getDefaultHostnameVerifier().verify(hostname, session))
            throw new SSLPeerUnverifiedException("Cannot verify hostname: " + hostname);
    }
    public Socket createSocket() throws IOException { return configure(delegate().createSocket(), null); }
    public Socket createSocket(String host, int port) throws IOException { return configure(delegate().createSocket(host, port), host); }
    public Socket createSocket(String host, int port, InetAddress local, int localPort) throws IOException {
        return configure(delegate().createSocket(host, port, local, localPort), host);
    }
    public Socket createSocket(InetAddress host, int port) throws IOException { return configure(delegate().createSocket(host, port), null); }
    public Socket createSocket(InetAddress host, int port, InetAddress local, int localPort) throws IOException {
        return configure(delegate().createSocket(host, port, local, localPort), null);
    }
    public Socket createSocket(Socket socket, String host, int port, boolean autoClose) throws IOException {
        return configure(delegate().createSocket(socket, host, port, autoClose), host);
    }
    public String[] getDefaultCipherSuites() { return delegate().getDefaultCipherSuites(); }
    public String[] getSupportedCipherSuites() { return delegate().getSupportedCipherSuites(); }
}
