package com.android.location.provider;

/**
 * Compile-only (tools/build-split-app.py, "stubs"): the class of Android's com.android.location.provider library that the
 * stand-in's geocode provider extends, as Google Play services' does; the platform has it, the stand-in does not carry it.
 */
public abstract class GeocodeProvider {
    public GeocodeProvider() {}
    public abstract String onGetFromLocation(double latitude, double longitude, int maxResults, android.location.GeocoderParams params,
                                             java.util.List<android.location.Address> addrs);
    public abstract String onGetFromLocationName(String locationName, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                                                 double upperRightLongitude, int maxResults, android.location.GeocoderParams params,
                                                 java.util.List<android.location.Address> addrs);
    public android.os.IBinder getBinder() { return null; }
}
