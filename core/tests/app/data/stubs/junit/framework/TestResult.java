package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public class TestResult {
    public int runCount() { return 0; }
    public int failureCount() { return 0; }
    public int errorCount() { return 0; }
    public java.util.Enumeration<TestFailure> failures() { return null; }
    public java.util.Enumeration<TestFailure> errors() { return null; }
    public boolean wasSuccessful() { return false; }
}
