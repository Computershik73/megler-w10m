package android.security.keystore.recovery;

/** What the recoverable key store's service throws when it fails, as Android's (RecoveryController). */
public class InternalRecoveryServiceException extends java.security.GeneralSecurityException {
    public InternalRecoveryServiceException(String msg) { super(msg); }
    public InternalRecoveryServiceException(String message, Throwable cause) { super(message, cause); }
}
