package android.net.http;

import java.time.Duration;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Megler: Android 14's QuicOptions, written: what the builder is given is kept and said back. The generated stub dropped the
 * allowed hosts (its addAllowedQuicHost was a signature alone, said in the log when the Play Store's Cronet wrapper handed
 * its hosts to HttpEngine) and said no cache size was ever set. Megler's engine has no QUIC: the options change nothing.
 */
public class QuicOptions {
    private final Set<String> allowedHosts;
    private final String handshakeUserAgent;
    private final Duration idleConnectionTimeout;
    private final boolean hasCacheSize;
    private final int cacheSize;

    /** @hide Android's hidden form of new Builder(). */
    public static Builder builder() {
        return new Builder();
    }

    protected QuicOptions() {
        this(new Builder());
    }

    QuicOptions(Builder b) {
        allowedHosts = Collections.unmodifiableSet(new LinkedHashSet<>(b.allowedHosts));
        handshakeUserAgent = b.handshakeUserAgent;
        idleConnectionTimeout = b.idleConnectionTimeout;
        hasCacheSize = b.hasCacheSize;
        cacheSize = b.cacheSize;
    }

    public Set<String> getAllowedQuicHosts() {
        return allowedHosts;
    }

    public String getHandshakeUserAgent() {
        return handshakeUserAgent;
    }

    public Duration getIdleConnectionTimeout() {
        return idleConnectionTimeout;
    }

    public boolean hasInMemoryServerConfigsCacheSize() {
        return hasCacheSize;
    }

    public int getInMemoryServerConfigsCacheSize() {
        if (!hasCacheSize) throw new IllegalStateException("InMemoryServerConfigsCacheSize is not set");
        return cacheSize;
    }

    public static final class Builder {
        private final Set<String> allowedHosts = new LinkedHashSet<>();
        private String handshakeUserAgent;
        private Duration idleConnectionTimeout;
        private boolean hasCacheSize;
        private int cacheSize;

        public Builder() {}

        public Builder addAllowedQuicHost(String host) {
            allowedHosts.add(host);
            return this;
        }

        public Builder setHandshakeUserAgent(String handshakeUserAgent) {
            this.handshakeUserAgent = handshakeUserAgent;
            return this;
        }

        public Builder setIdleConnectionTimeout(Duration idleConnectionTimeout) {
            this.idleConnectionTimeout = idleConnectionTimeout;
            return this;
        }

        public Builder setInMemoryServerConfigsCacheSize(int size) {
            hasCacheSize = true;
            cacheSize = size;
            return this;
        }

        public QuicOptions build() {
            return new QuicOptions(this);
        }
    }
}
