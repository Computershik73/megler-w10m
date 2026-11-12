// The generated exception discarded its code and message. DroidGuard examines the cause of an
// unsupported attestation, so preserve Android's internal error as well as the public classification
// for the unimplemented KeyMint operation our provider reports. Other codes use Android's fallback
// classification until their individual failure information is implemented here.
package android.security;

public class KeyStoreException extends Exception {
    public static final int ERROR_ATTESTATION_CHALLENGE_TOO_LARGE = 9;
    public static final int ERROR_ATTESTATION_KEYS_UNAVAILABLE = 16;
    public static final int ERROR_DEVICE_REQUIRES_UPGRADE_FOR_ATTESTATION = 17;
    public static final int ERROR_ID_ATTESTATION_FAILURE = 8;
    public static final int ERROR_INCORRECT_USAGE = 13;
    public static final int ERROR_INTERNAL_SYSTEM_ERROR = 4;
    public static final int ERROR_KEYMINT_FAILURE = 10;
    public static final int ERROR_KEYSTORE_FAILURE = 11;
    public static final int ERROR_KEYSTORE_UNINITIALIZED = 3;
    public static final int ERROR_KEY_CORRUPTED = 7;
    public static final int ERROR_KEY_DOES_NOT_EXIST = 6;
    public static final int ERROR_KEY_NOT_TEMPORALLY_VALID = 14;
    public static final int ERROR_KEY_OPERATION_EXPIRED = 15;
    public static final int ERROR_OTHER = 1;
    public static final int ERROR_PERMISSION_DENIED = 5;
    public static final int ERROR_UNIMPLEMENTED = 12;
    public static final int ERROR_USER_AUTHENTICATION_REQUIRED = 2;
    public static final int RETRY_AFTER_NEXT_REBOOT = 4;
    public static final int RETRY_NEVER = 1;
    public static final int RETRY_WHEN_CONNECTIVITY_AVAILABLE = 3;
    public static final int RETRY_WITH_EXPONENTIAL_BACKOFF = 2;
    public static final int RKP_FETCHING_PENDING_CONNECTIVITY = 3;
    public static final int RKP_FETCHING_PENDING_SOFTWARE_REBOOT = 4;
    public static final int RKP_SERVER_REFUSED_ISSUANCE = 2;
    public static final int RKP_SUCCESS = 0;
    public static final int RKP_TEMPORARILY_UNAVAILABLE = 1;
    private final int mErrorCode;

    public KeyStoreException(int errorCode, String message) {
        super(message);
        mErrorCode = errorCode;
    }
    public KeyStoreException(int errorCode, String message, int rkpStatus) {
        this(errorCode, message);
    }
    public KeyStoreException(int errorCode, String message, String keystoreErrorMessage) {
        this(errorCode, message + " (internal Keystore code: " + errorCode + " message: " + keystoreErrorMessage + ")");
    }
    public int getErrorCode() { return mErrorCode; }
    public int getNumericErrorCode() {
        return mErrorCode == -100 ? ERROR_UNIMPLEMENTED : mErrorCode > 0 ? ERROR_KEYSTORE_FAILURE : ERROR_KEYMINT_FAILURE;
    }
    public static boolean hasFailureInfoForError(int errorCode) { return errorCode == -100; }
    public boolean isSystemError() { return mErrorCode == -100; }
    public boolean isTransientFailure() { return false; }
    public boolean requiresUserAuthentication() { return false; }
    public int getRetryPolicy() { return RETRY_NEVER; }
    @Override public String toString() {
        return super.toString() + " (public error code: " + getNumericErrorCode()
                + " internal Keystore code: " + mErrorCode + ")";
    }
}
