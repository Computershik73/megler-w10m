// Overlay of core/framework/gen-src/android/accounts/IAccountAuthenticatorResponse.java (tools/build-framework.sh).
// It differs in having Android's methods, and a Stub: the generated stub was an empty interface, and nothing could answer
// an account request (android.accounts.AccountManager, megler.Accounts). An authenticator and the account manager are in
// one process here, so a response is a Binder that is the response itself (queryLocalInterface); there is no proxy.
package android.accounts;

import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;

public interface IAccountAuthenticatorResponse extends android.os.IInterface {
    void onResult(Bundle value) throws RemoteException;
    void onRequestContinued() throws RemoteException;
    void onError(int errorCode, String errorMessage) throws RemoteException;

    abstract class Stub extends android.os.Binder implements IAccountAuthenticatorResponse {
        public static final String DESCRIPTOR = "android.accounts.IAccountAuthenticatorResponse";
        public Stub() { attachInterface(this, DESCRIPTOR); }
        public static IAccountAuthenticatorResponse asInterface(IBinder b) {
            if (b == null) return null;
            android.os.IInterface local = b.queryLocalInterface(DESCRIPTOR);
            return local instanceof IAccountAuthenticatorResponse ? (IAccountAuthenticatorResponse) local : null;
        }
        @Override public IBinder asBinder() { return this; }
    }
}
