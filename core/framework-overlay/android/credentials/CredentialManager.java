// Overlay of core/framework/gen-src/android/credentials/CredentialManager.java (tools/build-framework.sh): the original's is
// a signature, whose getCredential never called its callback. Android 14's Credential Manager asks the credential providers
// the user has (password managers, Google's passkeys) and answers on the executor it is given; here there are none, and it
// answers as Android does with none: no credential (GetCredentialException TYPE_NO_CREDENTIAL), nothing to create a credential
// with (CreateCredentialException TYPE_NO_CREATE_OPTIONS), and a cleared state. AndroidX's Credential Manager, which takes
// this one on Android 14 and later, turns the first into NoCredentialException, and apps show their own sign-in form then.
// Avito's sign-in screen called getCredential and waited for the answer for ever (package 670, a Lumia 650).
package android.credentials;

import android.content.ComponentName;
import android.content.Context;
import android.os.CancellationSignal;
import android.os.OutcomeReceiver;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

@SuppressWarnings({"unchecked", "rawtypes"})
public final class CredentialManager {
    public static final String DEVICE_CONFIG_ENABLE_CREDENTIAL_MANAGER = "enable_credential_manager";
    public static final String EXTRA_AUTOFILL_RESULT_RECEIVER = "android.credentials.AUTOFILL_RESULT_RECEIVER";
    public static final int PROVIDER_FILTER_ALL_PROVIDERS = 0;
    public static final int PROVIDER_FILTER_SYSTEM_PROVIDERS_ONLY = 1;
    public static final int PROVIDER_FILTER_USER_PROVIDERS_INCLUDING_HIDDEN = 3;
    public static final int PROVIDER_FILTER_USER_PROVIDERS_ONLY = 2;
    public static final String TAG = "CredentialManager";

    public CredentialManager(Context context, ICredentialManager service) {}
    CredentialManager() { this((Context) null, (ICredentialManager) null); }

    public static boolean isCredentialDescriptionApiEnabled(Context context) { return false; }
    public static boolean isServiceEnabled(Context context) { return false; }

    public void getCredential(Context context, GetCredentialRequest request, CancellationSignal cancel, Executor executor, OutcomeReceiver callback) {
        noCredential(cancel, executor, callback);
    }

    public void getCredential(Context context, PrepareGetCredentialResponse.PendingGetCredentialHandle handle, CancellationSignal cancel, Executor executor,
                              OutcomeReceiver callback) {
        noCredential(cancel, executor, callback);
    }

    public void prepareGetCredential(GetCredentialRequest request, CancellationSignal cancel, Executor executor, OutcomeReceiver callback) {
        noCredential(cancel, executor, callback);
    }

    public void getCandidateCredentials(GetCredentialRequest request, String callingPackage, CancellationSignal cancel, Executor executor,
                                        OutcomeReceiver callback, android.os.IBinder clientCallback) {
        noCredential(cancel, executor, callback);
    }

    public void createCredential(Context context, CreateCredentialRequest request, CancellationSignal cancel, Executor executor, OutcomeReceiver callback) {
        answer(cancel, executor, () -> callback.onError(new CreateCredentialException(CreateCredentialException.TYPE_NO_CREATE_OPTIONS,
                "No create options available: there is no credential provider")));
    }

    public void clearCredentialState(ClearCredentialStateRequest request, CancellationSignal cancel, Executor executor, OutcomeReceiver callback) {
        answer(cancel, executor, () -> callback.onResult(null));
    }

    public List getCredentialProviderServices(int userId, int providerFilter) { return new ArrayList(); }
    public List getCredentialProviderServicesForTesting(int providerFilter) { return new ArrayList(); }
    public boolean isEnabledCredentialProviderService(ComponentName componentName) { return false; }
    public void registerCredentialDescription(RegisterCredentialDescriptionRequest request) {}
    public void setEnabledProviders(List primaryServices, List providers, int userId, Executor executor, OutcomeReceiver callback) {}
    public void unregisterCredentialDescription(UnregisterCredentialDescriptionRequest request) {}

    private static void noCredential(CancellationSignal cancel, Executor executor, OutcomeReceiver callback) {
        answer(cancel, executor, () -> callback.onError(new GetCredentialException(GetCredentialException.TYPE_NO_CREDENTIAL,
                "No credentials available: there is no credential provider")));
    }

    /** The answer, on the executor given, unless the request was cancelled first (Android says nothing then). */
    private static void answer(CancellationSignal cancel, Executor executor, Runnable callback) {
        if (cancel != null && cancel.isCanceled()) return;
        if (executor == null) callback.run();
        else executor.execute(callback);
    }

    public static abstract class GetCredentialTransportPendingUseCase extends IGetCredentialCallback.Stub {
        protected GetCredentialTransportPendingUseCase() { super(); }
    }
}
