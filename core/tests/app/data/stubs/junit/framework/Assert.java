package junit.framework;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps junit-* call of android.test.base, which an app made for Android 10 or before has on its class path. */
public class Assert {
    protected Assert() {}
    public static void assertTrue(boolean condition) {}
    public static void assertTrue(String message, boolean condition) {}
    public static void assertFalse(String message, boolean condition) {}
    public static void assertEquals(int expected, int actual) {}
    public static void assertEquals(String message, String expected, String actual) {}
    public static void assertEquals(double expected, double actual, double delta) {}
    public static void assertNull(Object object) {}
    public static void assertSame(Object expected, Object actual) {}
    public static void fail(String message) {}
}
