package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public class ComparisonFailure extends AssertionFailedError {
    public ComparisonFailure(String message, String expected, String actual) {}
    public String getExpected() { return null; }
    public String getActual() { return null; }
}
