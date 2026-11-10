package android.location;

/** Compile-only (tools/build-test-app.sh, "stubs"): what gms-host calls of Android 13's class, its listener forms among it. */
public final class Geocoder {
    public interface GeocodeListener {
        void onGeocode(java.util.List<Address> addresses);
        default void onError(String errorMessage) {}
    }
    public Geocoder(android.content.Context context) {}
    public Geocoder(android.content.Context context, java.util.Locale locale) {}
    public static boolean isPresent() { return false; }
    public java.util.List<Address> getFromLocation(double latitude, double longitude, int maxResults) throws java.io.IOException { return null; }
    public void getFromLocation(double latitude, double longitude, int maxResults, GeocodeListener listener) {}
    public java.util.List<Address> getFromLocationName(String locationName, int maxResults) throws java.io.IOException { return null; }
    public java.util.List<Address> getFromLocationName(String locationName, int maxResults, double lowerLeftLatitude, double lowerLeftLongitude,
                                                     double upperRightLatitude, double upperRightLongitude) throws java.io.IOException { return null; }
    public void getFromLocationName(String locationName, int maxResults, GeocodeListener listener) {}
    public void getFromLocationName(String locationName, int maxResults, double lowerLeftLatitude, double lowerLeftLongitude, double upperRightLatitude,
                                    double upperRightLongitude, GeocodeListener listener) {}
}
