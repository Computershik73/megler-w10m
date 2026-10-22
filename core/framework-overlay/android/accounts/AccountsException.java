// Overlay of core/framework/gen-src/android/accounts/AccountsException.java (tools/build-framework.sh).
// It differs in keeping its message and cause, as Android's does: the generated stub dropped both, and an account request
// that failed (AuthenticatorException, NetworkErrorException, OperationCanceledException) said nothing of why.
package android.accounts;

public class AccountsException extends Exception {
    public AccountsException() { super(); }
    public AccountsException(String message) { super(message); }
    public AccountsException(String message, Throwable cause) { super(message, cause); }
    public AccountsException(Throwable cause) { super(cause); }
}
