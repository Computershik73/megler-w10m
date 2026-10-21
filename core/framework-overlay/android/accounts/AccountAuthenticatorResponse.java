// Overlay of core/framework/gen-src/android/accounts/AccountAuthenticatorResponse.java (tools/build-framework.sh).
// It differs in being Android's: the generated stub dropped every answer, so an authenticator's activity (microG's sign-in)
// could never tell the account manager how a request ended. As on Android, it holds the account manager's response and
// travels in an intent as that response's binder (in this process, the response itself).
package android.accounts;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;

public class AccountAuthenticatorResponse implements Parcelable {
    private final IAccountAuthenticatorResponse mAccountAuthenticatorResponse;

    public AccountAuthenticatorResponse(IAccountAuthenticatorResponse response) { mAccountAuthenticatorResponse = response; }
    public AccountAuthenticatorResponse(Parcel parcel) { mAccountAuthenticatorResponse = IAccountAuthenticatorResponse.Stub.asInterface(parcel.readStrongBinder()); }

    public void onResult(Bundle result) {
        try { if (mAccountAuthenticatorResponse != null) mAccountAuthenticatorResponse.onResult(result); } catch (RemoteException e) { }
    }
    public void onRequestContinued() {
        try { if (mAccountAuthenticatorResponse != null) mAccountAuthenticatorResponse.onRequestContinued(); } catch (RemoteException e) { }
    }
    public void onError(int errorCode, String errorMessage) {
        try { if (mAccountAuthenticatorResponse != null) mAccountAuthenticatorResponse.onError(errorCode, errorMessage); } catch (RemoteException e) { }
    }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) { dest.writeStrongBinder(mAccountAuthenticatorResponse == null ? null : mAccountAuthenticatorResponse.asBinder()); }

    public static final Creator<AccountAuthenticatorResponse> CREATOR = new Creator<AccountAuthenticatorResponse>() {
        public AccountAuthenticatorResponse createFromParcel(Parcel source) { return new AccountAuthenticatorResponse(source); }
        public AccountAuthenticatorResponse[] newArray(int size) { return new AccountAuthenticatorResponse[size]; }
    };
}
