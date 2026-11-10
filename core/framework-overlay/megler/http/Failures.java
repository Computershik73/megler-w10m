package megler.http;

import android.net.http.NetworkException;
import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.ConnectException;
import java.net.NoRouteToHostException;
import java.net.ProtocolException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.net.UnknownServiceException;
import java.security.cert.CertificateException;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLPeerUnverifiedException;

/**
 * What Megler's HttpEngine fails with, as Android's HttpEngine does: a NetworkException with Android's error code for what
 * went wrong on the network, a CallbackException for an exception the app's callback or upload threw. The message says it
 * as Cronet does, with Chromium's name of the error ("Exception in BidirectionalStream: net::ERR_CONNECTION_REFUSED"), and
 * the Java exception it was is the cause.
 */
final class Failures {
    private Failures() {}

    static final class Network extends NetworkException {
        private final int code;

        Network(String message, Throwable cause, int code) {
            super(message, cause);
            this.code = code;
        }

        @Override
        public int getErrorCode() {
            return code;
        }

        /** Cronet's rule: a failure the same request may meet no more at once, tried again. */
        @Override
        public boolean isImmediatelyRetryable() {
            switch (code) {
                case ERROR_NETWORK_CHANGED:
                case ERROR_TIMED_OUT:
                case ERROR_CONNECTION_CLOSED:
                case ERROR_CONNECTION_TIMED_OUT:
                case ERROR_CONNECTION_RESET:
                    return true;
                default:
                    return false;
            }
        }
    }

    static final class Callback extends android.net.http.CallbackException {
        Callback(String message, Throwable cause) {
            super(message, cause);
        }
    }

    /** The server's TLS took no HTTP/2 (ALPN), which a bidirectional stream needs. */
    static final class NoHttp2 extends IOException {
        NoHttp2(String host, String chosen) {
            super(host + " did not take HTTP/2 (ALPN " + (chosen == null || chosen.isEmpty() ? "none" : chosen) + ")");
        }
    }

    /** `e`, met by `where` ("BidirectionalStream", "UrlRequest"), as Android's NetworkException; `connecting` while the
     *  connection was being made (a time-out then is the connection's). */
    static Network network(String where, Throwable e, boolean connecting) {
        String name;
        int code;
        Throwable inner = e instanceof H2.Ended && e.getCause() != null ? e.getCause() : e;
        String m = String.valueOf(inner.getMessage());
        if (e instanceof H2.Reset) {
            code = NetworkException.ERROR_OTHER;
            name = ((H2.Reset) e).code == H2.REFUSED_STREAM ? "net::ERR_HTTP2_SERVER_REFUSED_STREAM" : "net::ERR_HTTP2_PROTOCOL_ERROR";
        } else if (e instanceof H2.Ended && ((H2.Ended) e).code > 0 && !((H2.Ended) e).refused) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_HTTP2_PROTOCOL_ERROR";
        } else if (inner instanceof UnknownHostException) {
            code = NetworkException.ERROR_HOSTNAME_NOT_RESOLVED;
            name = "net::ERR_NAME_NOT_RESOLVED";
        } else if (inner instanceof NoRouteToHostException || m.contains("EHOSTUNREACH")) {
            code = NetworkException.ERROR_ADDRESS_UNREACHABLE;
            name = "net::ERR_ADDRESS_UNREACHABLE";
        } else if (m.contains("ENETUNREACH") || m.contains("ENETDOWN")) {
            code = NetworkException.ERROR_INTERNET_DISCONNECTED;
            name = "net::ERR_INTERNET_DISCONNECTED";
        } else if (inner instanceof SocketTimeoutException || m.contains("ETIMEDOUT")) {
            code = connecting ? NetworkException.ERROR_CONNECTION_TIMED_OUT : NetworkException.ERROR_TIMED_OUT;
            name = connecting ? "net::ERR_CONNECTION_TIMED_OUT" : "net::ERR_TIMED_OUT";
        } else if (inner instanceof ConnectException) {
            code = NetworkException.ERROR_CONNECTION_REFUSED;
            name = "net::ERR_CONNECTION_REFUSED";
        } else if (inner instanceof SSLPeerUnverifiedException) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_CERT_COMMON_NAME_INVALID";
        } else if (inner instanceof SSLHandshakeException && causedBy(inner, CertificateException.class)) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_CERT_AUTHORITY_INVALID";
        } else if (inner instanceof SSLException) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_SSL_PROTOCOL_ERROR";
        } else if (inner instanceof NoHttp2) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_ALPN_NEGOTIATION_FAILED";
        } else if (inner instanceof UnknownServiceException) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_CLEARTEXT_NOT_PERMITTED";
        } else if (inner instanceof ProtocolException) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_INVALID_HTTP_RESPONSE";
        } else if (inner instanceof SocketException && (m.contains("reset") || m.contains("ECONNRESET") || m.contains("EPIPE") || m.contains("Broken pipe"))) {
            code = NetworkException.ERROR_CONNECTION_RESET;
            name = "net::ERR_CONNECTION_RESET";
        } else if (e instanceof H2.Ended || inner instanceof EOFException || m.contains("unexpected end of stream")) {
            code = NetworkException.ERROR_CONNECTION_CLOSED;
            name = "net::ERR_CONNECTION_CLOSED";
        } else if (inner instanceof InterruptedIOException) {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_ABORTED";
        } else {
            code = NetworkException.ERROR_OTHER;
            name = "net::ERR_FAILED";
        }
        return new Network("Exception in " + where + ": " + name + " (" + e.getMessage() + ")", e, code);
    }

    private static boolean causedBy(Throwable t, Class<?> type) {
        for (int i = 0; t != null && i < 16; i++, t = t.getCause()) if (type.isInstance(t)) return true;
        return false;
    }
}
