// Overlay of core/framework/src/megler/PlayStore.java (tools/build-framework.sh).
// It differs in two more services it answers: Play Integrity's, the classic one (BIND_INTEGRITY_SERVICE,
// IIntegrityService.requestIntegrityToken) and the standard one (BIND_EXPRESS_INTEGRITY_SERVICE, IExpressIntegrityService:
// warm up, request), each answering every request with the error API_NOT_AVAILABLE (-1), as Play does on a phone where the
// Integrity API is not available. The rest is the original's, unchanged.
//
// Why. An integrity token is a verdict Google's servers sign about a certified Android phone, attested by its hardware; there
// is none to give here, and none is made up. Without these the bind failed ("Failed to bind to the service"), which the
// client library reports as CANNOT_BIND_TO_SERVICE after its retries, and an app may take that for a network failure that
// will pass: Claude, asked for its sign-in code, said it had no network. With them the app is told at once and plainly that
// the API is not available here, and what it does then is the app's (and its server's) to decide.
//
// The protocols, as the client library (com.google.android.play:integrity) speaks them -- read from its code in Avito 193
// (classic) and Firefox 158 (standard): a request is the descriptor, 1 and a Bundle, and the callback's binder, one way; the
// answer is the callback's descriptor, 1 and a Bundle with "error" (an IntegrityErrorCode) -- the classic callback's
// transaction 2; the standard one's 2 for a warm-up, 3 for a token, its Bundle with "is.error.remediable" too.
package megler;

import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * Services the Play Store app answers on a phone, bound by apps through Play's client libraries. Each is a binder speaking that
 * library's AIDL, answered in this process.
 */
final class PlayStore {
    private PlayStore() {}

    /** The binder for a bind intent aimed at the Play Store, or null. */
    static IBinder bind(Intent i) {
        if (i == null || !GooglePackages.VENDING.equals(i.getPackage())) return null;
        if ("com.google.android.finsky.ageverification.BIND".equals(i.getAction())) return AGE_SIGNALS;
        if ("com.google.android.play.core.integrityservice.BIND_INTEGRITY_SERVICE".equals(i.getAction())) return INTEGRITY;
        if ("com.google.android.play.core.expressintegrityservice.BIND_EXPRESS_INTEGRITY_SERVICE".equals(i.getAction())) return EXPRESS_INTEGRITY;
        return null;
    }

    /**
     * Play Age Signals (com.google.android.play:age-signals): checkAgeRange(package, options, callback). The answer is a result
     * without a user status -- what Play gives where no age law applies -- so games that ask go on as they would there.
     */
    private static final IBinder AGE_SIGNALS = new Binder() {
        private static final String SERVICE = "com.google.android.play.agesignals.protocol.IAgeSignalsService";
        private static final String CALLBACK = "com.google.android.play.agesignals.protocol.IAgeSignalsServiceCallback";
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != 1) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(SERVICE);
            data.readString();
            if (data.readInt() != 0) Bundle.CREATOR.createFromParcel(data);
            IBinder callback = data.readStrongBinder();
            if (callback == null) return true;
            android.util.Log.d("Megler", "Play Age Signals asked: answering that no age signal applies");
            Parcel p = Parcel.obtain();
            p.writeInterfaceToken(CALLBACK);
            p.writeInt(1);
            new Bundle().writeToParcel(p, 0);
            callback.transact(1, p, null, IBinder.FLAG_ONEWAY);
            return true;
        }
    };

    /** Play Integrity's error for an API that is not available (IntegrityErrorCode / StandardIntegrityErrorCode.API_NOT_AVAILABLE). */
    private static final int API_NOT_AVAILABLE = -1;

    /** A request's arguments read (its Bundle, then the callback), and the callback told API_NOT_AVAILABLE by `answer`. */
    private static boolean refuse(Parcel data, String service, String callbackDescriptor, int answer, boolean standard, String what) throws RemoteException {
        data.enforceInterface(service);
        if (data.readInt() != 0) Bundle.CREATOR.createFromParcel(data);
        IBinder callback = data.readStrongBinder();
        if (callback == null) return true;
        android.util.Log.i("Megler", "Play Integrity asked (" + what + "): answering API_NOT_AVAILABLE, there being no Integrity API here");
        Bundle b = new Bundle();
        b.putInt("error", API_NOT_AVAILABLE);
        if (standard) b.putBoolean("is.error.remediable", false);
        Parcel p = Parcel.obtain();
        p.writeInterfaceToken(callbackDescriptor);
        p.writeInt(1);
        b.writeToParcel(p, 0);
        callback.transact(answer, p, null, IBinder.FLAG_ONEWAY);
        p.recycle();
        return true;
    }

    /** Play Integrity, classic (IIntegrityService): requestIntegrityToken, transaction 2; the callback's answer, 2. */
    private static final IBinder INTEGRITY = new Binder() {
        private static final String SERVICE = "com.google.android.play.core.integrity.protocol.IIntegrityService";
        private static final String CALLBACK = "com.google.android.play.core.integrity.protocol.IIntegrityServiceCallback";
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != 2) return super.onTransact(code, data, reply, flags);
            return refuse(data, SERVICE, CALLBACK, 2, false, "a token");
        }
    };

    /** Play Integrity, standard (IExpressIntegrityService): warm up, transaction 2, answered by the callback's 2; request a token,
     *  3, answered by 3. */
    private static final IBinder EXPRESS_INTEGRITY = new Binder() {
        private static final String SERVICE = "com.google.android.play.core.integrity.protocol.IExpressIntegrityService";
        private static final String CALLBACK = "com.google.android.play.core.integrity.protocol.IExpressIntegrityServiceCallback";
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code == 2) return refuse(data, SERVICE, CALLBACK, 2, true, "standard, warming up");
            if (code == 3) return refuse(data, SERVICE, CALLBACK, 3, true, "standard, a token");
            return super.onTransact(code, data, reply, flags);
        }
    };
}
