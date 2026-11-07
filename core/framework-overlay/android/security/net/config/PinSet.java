// Megler: Android's, a <pin-set> (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/** A configuration's pins, checked until their expiration (milliseconds since 1970; Long.MAX_VALUE when none is said). */
public final class PinSet {
    public static final PinSet EMPTY_PINSET = new PinSet(Collections.<Pin>emptySet(), Long.MAX_VALUE);
    public final long expirationTime;
    public final Set<Pin> pins;

    public PinSet(Set<Pin> pins, long expirationTime) {
        if (pins == null) throw new NullPointerException("pins must not be null");
        this.pins = pins;
        this.expirationTime = expirationTime;
    }

    Set<String> getPinAlgorithms() {
        Set<String> algorithms = new HashSet<>();
        for (Pin pin : pins) algorithms.add(pin.digestAlgorithm);
        return algorithms;
    }
}
