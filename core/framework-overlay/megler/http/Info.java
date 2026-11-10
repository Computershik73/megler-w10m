package megler.http;

import android.net.http.HeaderBlock;
import android.net.http.UrlResponseInfo;
import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/** A response as Megler's HttpEngine tells it (Android's UrlResponseInfo): the URLs it went through, its status and headers,
 *  the protocol, and the bytes received, which grow as the body is read. */
final class Info extends UrlResponseInfo {
    private final List<String> chain;
    private final int code;
    private final String text;
    private final Headers headers;
    private final String protocol;
    private volatile long received;

    Info(List<String> chain, int code, String text, List<Map.Entry<String, String>> headers, String protocol, long received) {
        this.chain = Collections.unmodifiableList(new ArrayList<>(chain));
        this.code = code;
        this.text = text == null ? "" : text;
        this.headers = new Headers(headers);
        this.protocol = protocol == null ? "" : protocol;
        this.received = received;
    }

    void addReceived(long bytes) {
        received += bytes;
    }

    @Override public HeaderBlock getHeaders() { return headers; }
    @Override public int getHttpStatusCode() { return code; }
    @Override public String getHttpStatusText() { return text; }
    @Override public String getNegotiatedProtocol() { return protocol; }
    @Override public long getReceivedByteCount() { return received; }
    @Override public String getUrl() { return chain.get(chain.size() - 1); }
    @Override public List<String> getUrlChain() { return chain; }
    @Override public boolean wasCached() { return false; }

    @Override
    public String toString() {
        return "UrlResponseInfo@[" + Integer.toHexString(System.identityHashCode(this)) + "][" + getUrl() + "]: urlChain = " + chain
                + ", httpStatus = " + code + " " + text + ", headers = " + headers.getAsList() + ", wasCached = false, negotiatedProtocol = "
                + protocol + ", receivedByteCount = " + received;
    }

    static Map.Entry<String, String> entry(String name, String value) {
        return new AbstractMap.SimpleImmutableEntry<>(name, value);
    }

    /** Headers in the order they came, and by name without regard to case, as Cronet keeps them. */
    static final class Headers extends HeaderBlock {
        private final List<Map.Entry<String, String>> list;
        private Map<String, List<String>> map;

        Headers(List<Map.Entry<String, String>> list) {
            this.list = Collections.unmodifiableList(new ArrayList<>(list));
        }

        @Override
        public List<Map.Entry<String, String>> getAsList() {
            return list;
        }

        @Override
        public synchronized Map<String, List<String>> getAsMap() {
            if (map == null) {
                TreeMap<String, List<String>> m = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
                for (Map.Entry<String, String> e : list) {
                    List<String> values = m.get(e.getKey());
                    if (values == null) m.put(e.getKey(), values = new ArrayList<>());
                    values.add(e.getValue());
                }
                for (Map.Entry<String, List<String>> e : m.entrySet()) e.setValue(Collections.unmodifiableList(e.getValue()));
                map = Collections.unmodifiableMap(m);
            }
            return map;
        }

        @Override
        public String toString() {
            return list.toString();
        }
    }
}
