// Overlay of core/framework/gen-src/android/accounts/AccountAuthenticatorActivity.java (tools/build-framework.sh).
// It differs in being Android's: an authenticator's activity built on it hands its result (or that it was cancelled) to the
// response it was started with when it finishes. The generated stub kept the result and told no one, so the account
// manager's request waited for ever.
package android.accounts;

import android.app.Activity;
import android.os.Bundle;

public class AccountAuthenticatorActivity extends Activity {
    private AccountAuthenticatorResponse mAccountAuthenticatorResponse = null;
    private Bundle mResultBundle = null;

    public final void setAccountAuthenticatorResult(Bundle result) { mResultBundle = result; }

    @Override protected void onCreate(Bundle icicle) {
        super.onCreate(icicle);
        mAccountAuthenticatorResponse = getIntent().getParcelableExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE);
        if (mAccountAuthenticatorResponse != null) mAccountAuthenticatorResponse.onRequestContinued();
    }

    @Override public void finish() {
        if (mAccountAuthenticatorResponse != null) {
            if (mResultBundle != null) mAccountAuthenticatorResponse.onResult(mResultBundle);
            else mAccountAuthenticatorResponse.onError(AccountManager.ERROR_CODE_CANCELED, "canceled");
            mAccountAuthenticatorResponse = null;
        }
        super.finish();
    }
}
