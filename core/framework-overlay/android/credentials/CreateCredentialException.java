// Overlay of core/framework/gen-src/android/credentials/CreateCredentialException.java (tools/build-framework.sh): the
// original's is a signature, whose getType() said null and whose message was lost; AndroidX's Credential Manager reads the
// type (CredentialManager below gives it), so it is kept here as Android's keeps it.
package android.credentials;

public class CreateCredentialException extends Exception {
    public static final String TYPE_INTERRUPTED = "android.credentials.CreateCredentialException.TYPE_INTERRUPTED";
    public static final String TYPE_NO_CREATE_OPTIONS = "android.credentials.CreateCredentialException.TYPE_NO_CREATE_OPTIONS";
    public static final String TYPE_UNKNOWN = "android.credentials.CreateCredentialException.TYPE_UNKNOWN";
    public static final String TYPE_USER_CANCELED = "android.credentials.CreateCredentialException.TYPE_USER_CANCELED";

    private final String mType;

    public CreateCredentialException(String type) { this(type, null, null); }
    public CreateCredentialException(String type, String message) { this(type, message, null); }
    public CreateCredentialException(String type, Throwable cause) { this(type, null, cause); }
    public CreateCredentialException(String type, String message, Throwable cause) {
        super(message, cause);
        if (type == null || type.isEmpty()) throw new IllegalArgumentException("type must not be empty");
        mType = type;
    }
    CreateCredentialException() { this(TYPE_UNKNOWN); }

    public String getType() { return mType; }
}
