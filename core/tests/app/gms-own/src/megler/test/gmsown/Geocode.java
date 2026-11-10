package megler.test.gmsown;

import android.app.Service;
import android.content.Intent;
import android.location.Address;
import android.location.GeocoderParams;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import com.android.location.provider.GeocodeProvider;
import java.util.List;
import java.util.Locale;

/**
 * Google Play services' geocode provider, as theirs is made (GmsCore's GeocodeService, not exported: the system binds it for
 * an app's Geocoder): a service whose binder is a com.android.location.provider.GeocodeProvider's. It says who asked
 * (GeocoderParams: the app's package and uid, its locale) and answers with one place, Red Square, in the request's locale:
 * at the point asked, or at its own for its name; south of the equator, or for another name, an error of its own.
 */
public class Geocode extends Service {
    private final Provider provider = new Provider();

    @Override
    public IBinder onBind(Intent intent) {
        Log.i("GmsOwnTest", "geocode provider bound " + (Looper.myLooper() == Looper.getMainLooper() ? "on the main thread" : "off the main thread"));
        return provider.getBinder();
    }

    static final class Provider extends GeocodeProvider {
        @Override
        public String onGetFromLocation(double latitude, double longitude, int maxResults, GeocoderParams params, List<Address> addrs) {
            Log.i("GmsOwnTest", "geocode at a point for " + who(params) + ", at most " + maxResults);
            if (latitude < 0) return "no place south of the equator";
            Address a = redSquare(params.getLocale());
            a.setLatitude(latitude);
            a.setLongitude(longitude);
            addrs.add(a);
            return null;
        }

        @Override
        public String onGetFromLocationName(String locationName, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                                            double upperRightLongitude, int maxResults, GeocoderParams params, List<Address> addrs) {
            Log.i("GmsOwnTest", "geocode of a name for " + who(params) + ", within " + lowerLeftLatitude + "," + lowerLeftLongitude + "," + upperRightLatitude + ","
                + upperRightLongitude + ", at most " + maxResults);
            if (!"Red Square".equals(locationName)) return "no place called " + locationName;
            Address a = redSquare(params.getLocale());
            a.setLatitude(55.7539);
            a.setLongitude(37.6208);
            addrs.add(a);
            return null;
        }

        private static String who(GeocoderParams p) {
            return p.getClientPackage() + " (" + (p.getClientUid() == android.os.Process.myUid() ? "the app's uid" : "uid " + p.getClientUid()) + ") in "
                + p.getLocale() + " off the main thread " + (Looper.myLooper() != Looper.getMainLooper());
        }

        private static Address redSquare(Locale locale) {
            Address a = new Address(locale);
            a.setFeatureName("Red Square");
            a.setAddressLine(0, "Red Square");
            a.setAddressLine(1, "Moscow, 109012");
            a.setThoroughfare("Red Square");
            a.setLocality("Moscow");
            a.setAdminArea("Moscow");
            a.setPostalCode("109012");
            a.setCountryCode("RU");
            a.setCountryName("Russia");
            return a;
        }
    }
}
