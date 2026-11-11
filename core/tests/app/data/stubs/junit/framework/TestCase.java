package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public abstract class TestCase extends Assert implements Test {
    public TestCase() {}
    public TestCase(String name) {}
    public int countTestCases() { return 0; }
    public void run(TestResult result) {}
    public TestResult run() { return null; }
    protected void setUp() throws Exception {}
    protected void tearDown() throws Exception {}
    public String getName() { return null; }
}
