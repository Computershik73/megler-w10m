// Overlay of core/framework/gen-src/android/credentials/GetCredentialException.java (tools/build-framework.sh): the
// original's is a signature, whose getType() said null and whose message was lost. The type is what AndroidX's Credential
// Manager reads to tell "no credential" from a failure (androidx.credentials.exceptions.NoCredentialException), so it is kept
// here as Android's keeps it (CredentialManager below gives it).
package android.credentials;

public class GetCredentialException extends Exception {
    public static final String TYPE_INTERRUPTED = "android.credentials.GetCredentialException.TYPE_INTERRUPTED";
    public static final String TYPE_NO_CREDENTIAL = "android.credentials.GetCredentialException.TYPE_NO_CREDENTIAL";
    public static final String TYPE_UNKNOWN = "android.credentials.GetCredentialException.TYPE_UNKNOWN";
    public static final String TYPE_USER_CANCELED = "android.credentials.GetCredentialException.TYPE_USER_CANCELED";

    private final String mType;

    public GetCredentialException(String type) { this(type, null, null); }
    public GetCredentialException(String type, String message) { this(type, message, null); }
    public GetCredentialException(String type, Throwable cause) { this(type, null, cause); }
    public GetCredentialException(String type, String message, Throwable cause) {
        super(message, cause);
        if (type == null || type.isEmpty()) throw new IllegalArgumentException("type must not be empty");
        mType = type;
    }
    GetCredentialException() { this(TYPE_UNKNOWN); }

    public String getType() { return mType; }
}
