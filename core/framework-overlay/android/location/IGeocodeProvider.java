// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// AIDL interface of a geocode provider (Android 12 and later: one-way calls, the answer to a listener), which the binder of
// com.android.location.provider.GeocodeProvider implements and Android's location manager calls for an app's Geocoder. In
// Megler the provider, Google Play services' own, runs in the app, so the binder is a local one, found by its descriptor
// (queryLocalInterface) and called in place: no transaction is ever made of these calls.
package android.location;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;

/** A geocode provider's binder interface (hidden API): each request answered once, to its listener. */
public interface IGeocodeProvider extends IInterface {
    String DESCRIPTOR = "android.location.IGeocodeProvider";

    void getFromLocation(double latitude, double longitude, int maxResults, GeocoderParams params, IGeocodeListener listener);
    void getFromLocationName(String locationName, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                             double upperRightLongitude, int maxResults, GeocoderParams params, IGeocodeListener listener);

    /** The provider's side: its binder, local to the process. */
    abstract class Stub extends Binder implements IGeocodeProvider {
        public Stub() { attachInterface(this, DESCRIPTOR); }
        /** The provider a binder is, or null for one of another kind. */
        public static IGeocodeProvider asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IGeocodeProvider ? (IGeocodeProvider) local : null;
        }
        @Override public IBinder asBinder() { return this; }
    }
}
