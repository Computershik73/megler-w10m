// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/** Two strings that should have been equal (Assert.assertEquals of strings); its message shows where they differ. */
public class ComparisonFailure extends AssertionFailedError {
    private static final int MAX_CONTEXT_LENGTH = 20;
    private static final long serialVersionUID = 1L;

    private final String expected, actual;

    public ComparisonFailure(String message, String expected, String actual) {
        super(message);
        this.expected = expected;
        this.actual = actual;
    }

    /** The message it was made with, then the two strings with their difference in brackets (ComparisonCompactor). */
    @Override
    public String getMessage() {
        return new ComparisonCompactor(MAX_CONTEXT_LENGTH, expected, actual).compact(super.getMessage());
    }

    public String getActual() {
        return actual;
    }

    public String getExpected() {
        return expected;
    }
}
