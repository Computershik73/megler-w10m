// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/**
 * What a failed assertion throws: an AssertionError, as Android's is. Made with a message, a null one is kept as an empty
 * one (Assert.fail() without a message gives ""); made without, it has none.
 */
public class AssertionFailedError extends AssertionError {
    private static final long serialVersionUID = 1L;

    public AssertionFailedError() {
    }

    public AssertionFailedError(String message) {
        super(message != null ? message : "");
    }
}
