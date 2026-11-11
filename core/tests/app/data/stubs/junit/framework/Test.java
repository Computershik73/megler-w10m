package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public interface Test {
    int countTestCases();
    void run(TestResult result);
}
