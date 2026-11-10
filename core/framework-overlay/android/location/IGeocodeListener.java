// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// AIDL interface by which a geocode provider answers one request (android.location.IGeocodeProvider): an error, or the
// addresses found. Local here, as the provider is (Google Play services', in the app's process).
package android.location;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import java.util.List;

/** The answer of a geocode provider to one request (hidden API): `error` null and the addresses, or why there are none. */
public interface IGeocodeListener extends IInterface {
    String DESCRIPTOR = "android.location.IGeocodeListener";

    void onResults(String error, List<Address> results);

    /** The asker's side: a binder local to the process. */
    abstract class Stub extends Binder implements IGeocodeListener {
        public Stub() { attachInterface(this, DESCRIPTOR); }
        public static IGeocodeListener asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IGeocodeListener ? (IGeocodeListener) local : null;
        }
        @Override public IBinder asBinder() { return this; }
    }
}
