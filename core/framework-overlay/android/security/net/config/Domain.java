// Megler: Android's, a <domain> of a <domain-config> (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import java.util.Locale;

/** A host name a configuration is for, in lower case, and whether its subdomains are too (includeSubdomains). */
public final class Domain {
    public final String hostname;
    public final boolean subdomainsIncluded;

    public Domain(String hostname, boolean subdomainsIncluded) {
        if (hostname == null) throw new NullPointerException("Hostname must not be null");
        this.hostname = hostname.toLowerCase(Locale.US);
        this.subdomainsIncluded = subdomainsIncluded;
    }

    @Override public int hashCode() { return hostname.hashCode() ^ (subdomainsIncluded ? 1231 : 1237); }

    @Override public boolean equals(Object other) {
        if (other == this) return true;
        if (!(other instanceof Domain)) return false;
        Domain d = (Domain) other;
        return d.subdomainsIncluded == subdomainsIncluded && d.hostname.equals(hostname);
    }
}
