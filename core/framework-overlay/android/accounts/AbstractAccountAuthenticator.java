// Overlay of core/framework/gen-src/android/accounts/AbstractAccountAuthenticator.java (tools/build-framework.sh).
// It differs in giving an authenticator's service something to return from onBind: the generated stub's getIBinder was
// null, and no account request could reach an authenticator (microG's for Google accounts, an app's own). The account manager
// is in the same process here (android.accounts.AccountManager), so the binder is the authenticator's own Transport, through
// which the account manager calls its methods directly; Android's goes through IAccountAuthenticator. The defaults of the
// methods that are not abstract are Android's.
package android.accounts;

import android.content.Context;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

public abstract class AbstractAccountAuthenticator {
    public static final String KEY_CUSTOM_TOKEN_EXPIRY = "android.accounts.expiry";

    private final Transport mTransport = new Transport();

    public AbstractAccountAuthenticator(Context context) { }

    /** The binder an authenticator's service hands out: in this process, the way to the authenticator itself. */
    public final class Transport extends Binder {
        public AbstractAccountAuthenticator authenticator() { return AbstractAccountAuthenticator.this; }
    }

    public final IBinder getIBinder() { return mTransport; }

    /** The authenticator behind a binder an authenticator's service returned, or null when it is not one. */
    public static AbstractAccountAuthenticator of(IBinder binder) {
        return binder instanceof Transport ? ((Transport) binder).authenticator() : null;
    }

    public abstract Bundle editProperties(AccountAuthenticatorResponse response, String accountType);
    public abstract Bundle addAccount(AccountAuthenticatorResponse response, String accountType, String authTokenType, String[] requiredFeatures,
            Bundle options) throws NetworkErrorException;
    public abstract Bundle confirmCredentials(AccountAuthenticatorResponse response, Account account, Bundle options) throws NetworkErrorException;
    public abstract Bundle getAuthToken(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException;
    public abstract String getAuthTokenLabel(String authTokenType);
    public abstract Bundle updateCredentials(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException;
    public abstract Bundle hasFeatures(AccountAuthenticatorResponse response, Account account, String[] features) throws NetworkErrorException;

    public Bundle getAccountRemovalAllowed(AccountAuthenticatorResponse response, Account account) throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
        return result;
    }
    public Bundle getAccountCredentialsForCloning(final AccountAuthenticatorResponse response, final Account account) throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
    public Bundle addAccountFromCredentials(final AccountAuthenticatorResponse response, Account account, Bundle accountCredentials)
            throws NetworkErrorException {
        final Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
    public Bundle startAddAccountSession(AccountAuthenticatorResponse response, String accountType, String authTokenType, String[] requiredFeatures,
            Bundle options) throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle startUpdateCredentialsSession(AccountAuthenticatorResponse response, Account account, String authTokenType, Bundle options)
            throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle finishSession(AccountAuthenticatorResponse response, String accountType, Bundle sessionBundle) throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putInt(AccountManager.KEY_ERROR_CODE, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION);
        return result;
    }
    public Bundle isCredentialsUpdateSuggested(final AccountAuthenticatorResponse response, Account account, String statusToken)
            throws NetworkErrorException {
        Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
        return result;
    }
}
