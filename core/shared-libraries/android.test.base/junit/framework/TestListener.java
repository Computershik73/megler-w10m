// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/** Hears the progress of a run from a TestResult it was added to. */
public interface TestListener {
    /** A test ended in an exception that was not an AssertionFailedError. */
    void addError(Test test, Throwable e);

    /** An assertion of a test failed. */
    void addFailure(Test test, AssertionFailedError e);

    void endTest(Test test);

    void startTest(Test test);
}
