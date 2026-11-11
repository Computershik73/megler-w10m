// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

import java.io.PrintWriter;
import java.io.StringWriter;

/** A test that failed or ended in an error, and what it threw. */
public class TestFailure {
    protected Test fFailedTest;
    protected Throwable fThrownException;

    public TestFailure(Test failedTest, Throwable thrownException) {
        fFailedTest = failedTest;
        fThrownException = thrownException;
    }

    public Test failedTest() {
        return fFailedTest;
    }

    public Throwable thrownException() {
        return fThrownException;
    }

    /** "the test: the message". */
    @Override
    public String toString() {
        return fFailedTest + ": " + fThrownException.getMessage();
    }

    /** What was thrown, with its stack, as printStackTrace writes it. */
    public String trace() {
        StringWriter out = new StringWriter();
        thrownException().printStackTrace(new PrintWriter(out));
        return out.toString();
    }

    public String exceptionMessage() {
        return thrownException().getMessage();
    }

    /** An assertion's failure rather than an error. */
    public boolean isFailure() {
        return thrownException() instanceof AssertionFailedError;
    }
}
