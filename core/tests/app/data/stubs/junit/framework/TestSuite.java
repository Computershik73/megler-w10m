package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public class TestSuite implements Test {
    public TestSuite(Class<?> theClass) {}
    public int countTestCases() { return 0; }
    public void run(TestResult result) {}
    public int testCount() { return 0; }
}
