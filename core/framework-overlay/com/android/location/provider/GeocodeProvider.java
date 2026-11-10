package com.android.location.provider;

import android.location.Address;
import android.location.GeocoderParams;
import android.location.IGeocodeListener;
import android.location.IGeocodeProvider;
import android.os.IBinder;
import java.util.ArrayList;
import java.util.List;

/**
 * A geocode provider of an unbundled service, of Android's com.android.location.provider library: Google Play services'
 * (GmsCore's GeocodeChimeraService, its legacy provider below Android 15) and microG's (OpenStreetMapNominatimGeocodeProvider)
 * extend it, and their service's onBind gives getBinder. Neither carries the class: the system's library has it, and without
 * it their geocode service could not be made. Each request of the binder is answered as Android's library answers it: the
 * subclass's onGetFromLocation or onGetFromLocationName fills a list and gives an error or null, and the listener hears both,
 * on the thread that asked (megler's android.location.Geocoder asks on a thread of its own, as a binder thread would).
 */
public abstract class GeocodeProvider {
    private final IGeocodeProvider.Stub mProvider = new IGeocodeProvider.Stub() {
        @Override public void getFromLocation(double latitude, double longitude, int maxResults, GeocoderParams params, IGeocodeListener listener) {
            List<Address> results = new ArrayList<>();
            String error = onGetFromLocation(latitude, longitude, maxResults, params, results);
            listener.onResults(error, results);
        }
        @Override public void getFromLocationName(String locationName, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                                                  double upperRightLongitude, int maxResults, GeocoderParams params, IGeocodeListener listener) {
            List<Address> results = new ArrayList<>();
            String error = onGetFromLocationName(locationName, lowerLeftLatitude, lowerLeftLongitude, upperRightLatitude, upperRightLongitude, maxResults,
                                                 params, results);
            listener.onResults(error, results);
        }
    };

    public GeocodeProvider() {}

    /** The places at a point, into `addrs`: null, or an error (Geocoder.getFromLocation). */
    public abstract String onGetFromLocation(double latitude, double longitude, int maxResults, GeocoderParams params, List<Address> addrs);
    /** The places of a name, within the bounds where they are not all 0, into `addrs`: null, or an error (Geocoder.getFromLocationName). */
    public abstract String onGetFromLocationName(String locationName, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                                                 double upperRightLongitude, int maxResults, GeocoderParams params, List<Address> addrs);

    /** The provider's binder, for its service's onBind. */
    public IBinder getBinder() { return mProvider; }
}
