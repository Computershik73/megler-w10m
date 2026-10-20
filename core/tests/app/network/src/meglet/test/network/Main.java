package meglet.test.network;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import android.net.ssl.SSLSockets;
import java.net.InetSocketAddress;
import java.util.Arrays;

/** Explicit on-device Internet diagnostic, kept out of the offline test suite.
 * Uses the platform trust store and hostname verification without overrides. */
public class Main extends Activity {
    private TextView text;
    private final StringBuilder results = new StringBuilder();

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
    }

    private void modernTls() throws Exception {
        SSLParameters parameters = new SSLParameters();
        check(parameters.getApplicationProtocols().length == 0, "default ALPN");
        String[] protocols = { "h2", "http/1.1" };
        parameters.setApplicationProtocols(protocols);
        protocols[0] = "changed";
        String[] copy = parameters.getApplicationProtocols();
        copy[0] = "changed";
        check("h2".equals(parameters.getApplicationProtocols()[0]), "ALPN defensive copies");
        for (String[] invalid : new String[][] { null, { null }, { "" } }) {
            boolean rejected = false;
            try { parameters.setApplicationProtocols(invalid); }
            catch (IllegalArgumentException expected) { rejected = true; }
            check(rejected, "invalid ALPN accepted");
        }
        SSLEngine engine = SSLContext.getDefault().createSSLEngine();
        engine.setSSLParameters(parameters);
        check(Arrays.equals(engine.getSSLParameters().getApplicationProtocols(), new String[] { "h2", "http/1.1" }), "engine ALPN round trip");
        try (SSLSocket socket = (SSLSocket) SSLSocketFactory.getDefault().createSocket()) {
            check(SSLSockets.isSupportedSocket(socket), "Conscrypt socket supported");
            SSLSockets.setUseSessionTickets(socket, true);
            socket.connect(new InetSocketAddress("www.google.com", 443), 10000);
            socket.setSoTimeout(10000);
            parameters.setEndpointIdentificationAlgorithm("HTTPS");
            parameters.setServerNames(java.util.Collections.singletonList(new javax.net.ssl.SNIHostName("www.google.com")));
            socket.setSSLParameters(parameters);
            SSLParameters actual = socket.getSSLParameters();
            check(Arrays.equals(actual.getApplicationProtocols(), new String[] { "h2", "http/1.1" }), "socket ALPN round trip");
            check("HTTPS".equals(actual.getEndpointIdentificationAlgorithm()), "endpoint verification retained");
            socket.startHandshake();
            String negotiated = socket.getApplicationProtocol();
            check("h2".equals(negotiated), "HTTP/2 ALPN negotiation: " + negotiated);
            say("Android 10 TLS API OK ALPN=" + negotiated + " cipher=" + socket.getSession().getCipherSuite());
        }
    }

    private void say(String line) {
        Log.i("NetworkTest", line);
        results.append(line).append('\n');
        final String snapshot = results.toString();
        runOnUiThread(() -> text.setText(snapshot));
    }

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        text = new TextView(this);
        text.setText("Checking DNS, HTTP and HTTPS...");
        setContentView(text);
        new Thread(() -> {
            try { modernTls(); }
            catch (Throwable e) { say("Android 10 TLS API FAILED " + e); Log.e("NetworkTest", "modern TLS failed", e); }
            for (String host : new String[] { "example.com", "www.google.com" }) {
                try {
                    InetAddress[] addresses = InetAddress.getAllByName(host);
                    say("DNS " + host + " OK addresses=" + addresses.length);
                } catch (Exception e) { say("DNS " + host + " FAILED " + e); }
            }
            for (String address : new String[] { "http://example.com/", "https://example.com/", "https://www.google.com/" }) {
                HttpURLConnection c = null;
                try {
                    c = (HttpURLConnection) new URL(address).openConnection();
                    c.setConnectTimeout(10000);
                    c.setReadTimeout(10000);
                    c.setInstanceFollowRedirects(false);
                    int status = c.getResponseCode();
                    String tls = c instanceof HttpsURLConnection ? " cipher=" + ((HttpsURLConnection)c).getCipherSuite() : "";
                    int firstByte = c.getInputStream().read();
                    say(address + " OK status=" + status + " body=" + (firstByte >= 0) + tls);
                } catch (Exception e) {
                    say(address + " FAILED " + e);
                    Log.e("NetworkTest", "request failed", e);
                } finally { if (c != null) c.disconnect(); }
            }
            say("DONE");
        }, "internet-check").start();
    }
}
