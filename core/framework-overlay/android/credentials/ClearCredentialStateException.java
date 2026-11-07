// Overlay of core/framework/gen-src/android/credentials/ClearCredentialStateException.java (tools/build-framework.sh): the
// original's is a signature, whose getType() said null and whose message was lost; kept here as Android's keeps them.
package android.credentials;

public class ClearCredentialStateException extends Exception {
    public static final String TYPE_UNKNOWN = "android.credentials.ClearCredentialStateException.TYPE_UNKNOWN";

    private final String mType;

    public ClearCredentialStateException(String type) { this(type, null, null); }
    public ClearCredentialStateException(String type, String message) { this(type, message, null); }
    public ClearCredentialStateException(String type, Throwable cause) { this(type, null, cause); }
    public ClearCredentialStateException(String type, String message, Throwable cause) {
        super(message, cause);
        if (type == null || type.isEmpty()) throw new IllegalArgumentException("type must not be empty");
        mType = type;
    }
    ClearCredentialStateException() { this(TYPE_UNKNOWN); }

    public String getType() { return mType; }
}
