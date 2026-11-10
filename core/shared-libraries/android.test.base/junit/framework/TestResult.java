// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Vector;

/**
 * What a run came to: how many tests ran, the failures (an AssertionFailedError) and the errors (anything else thrown), each
 * told to the listeners as it happens. A run asked to stop (stop) runs no further test of a suite.
 */
public class TestResult {
    protected Vector<TestFailure> fFailures = new Vector<>();
    protected Vector<TestFailure> fErrors = new Vector<>();
    protected Vector<TestListener> fListeners = new Vector<>();
    protected int fRunTests;
    private boolean stopped;

    public TestResult() {
    }

    public synchronized void addError(Test test, Throwable e) {
        fErrors.add(new TestFailure(test, e));
        for (TestListener l : listeners()) l.addError(test, e);
    }

    public synchronized void addFailure(Test test, AssertionFailedError e) {
        fFailures.add(new TestFailure(test, e));
        for (TestListener l : listeners()) l.addFailure(test, e);
    }

    public synchronized void addListener(TestListener listener) {
        fListeners.add(listener);
    }

    public synchronized void removeListener(TestListener listener) {
        fListeners.remove(listener);
    }

    /** The listeners as they are now: one may add or remove listeners while it is told something. */
    private synchronized List<TestListener> listeners() {
        return new ArrayList<>(fListeners);
    }

    public void startTest(Test test) {
        int cases = test.countTestCases();
        synchronized (this) {
            fRunTests += cases;
        }
        for (TestListener l : listeners()) l.startTest(test);
    }

    public void endTest(Test test) {
        for (TestListener l : listeners()) l.endTest(test);
    }

    /** A test case run bare (setUp, the test, tearDown), between its start and its end. */
    protected void run(final TestCase test) {
        startTest(test);
        runProtected(test, new Protectable() {
            @Override
            public void protect() throws Throwable {
                test.runBare();
            }
        });
        endTest(test);
    }

    /** Runs `p`: an AssertionFailedError it throws is a failure of `test`, anything else but ThreadDeath an error. */
    public void runProtected(Test test, Protectable p) {
        try {
            p.protect();
        } catch (AssertionFailedError e) {
            addFailure(test, e);
        } catch (ThreadDeath e) {
            throw e;
        } catch (Throwable e) {
            addError(test, e);
        }
    }

    public synchronized int runCount() {
        return fRunTests;
    }

    public synchronized int errorCount() {
        return fErrors.size();
    }

    public synchronized Enumeration<TestFailure> errors() {
        return fErrors.elements();
    }

    public synchronized int failureCount() {
        return fFailures.size();
    }

    public synchronized Enumeration<TestFailure> failures() {
        return fFailures.elements();
    }

    public synchronized boolean shouldStop() {
        return stopped;
    }

    public synchronized void stop() {
        stopped = true;
    }

    public synchronized boolean wasSuccessful() {
        return fFailures.isEmpty() && fErrors.isEmpty();
    }
}
