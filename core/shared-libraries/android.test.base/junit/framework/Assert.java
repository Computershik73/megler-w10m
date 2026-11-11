// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/**
 * The assertions, each throwing an AssertionFailedError (a ComparisonFailure for two strings) when it does not hold. The
 * messages are worded as Android's: "expected:<a> but was:<b>", "expected same:<a> was not:<b>", "expected not same",
 * "Expected: <null> but was: x", the caller's message and a space before them where it gave one.
 */
public class Assert {
    protected Assert() {
    }

    public static void assertTrue(String message, boolean condition) {
        if (!condition) fail(message);
    }

    public static void assertTrue(boolean condition) {
        assertTrue(null, condition);
    }

    public static void assertFalse(String message, boolean condition) {
        if (condition) fail(message);
    }

    public static void assertFalse(boolean condition) {
        assertFalse(null, condition);
    }

    public static void fail(String message) {
        throw new AssertionFailedError(message);
    }

    public static void fail() {
        fail(null);
    }

    /** Equal by equals(), or both null. */
    public static void assertEquals(String message, Object expected, Object actual) {
        if (same(expected, actual)) return;
        failNotEquals(message, expected, actual);
    }

    public static void assertEquals(Object expected, Object actual) {
        assertEquals(null, expected, actual);
    }

    /** Two strings: a ComparisonFailure where they differ, whose message shows the difference (ComparisonCompactor). */
    public static void assertEquals(String message, String expected, String actual) {
        if (same(expected, actual)) return;
        throw new ComparisonFailure(message != null ? message : "", expected, actual);
    }

    public static void assertEquals(String expected, String actual) {
        assertEquals(null, expected, actual);
    }

    /** Within delta of each other; the same value (NaN and NaN, an infinity and itself) whatever delta is. */
    public static void assertEquals(String message, double expected, double actual, double delta) {
        if (Double.compare(expected, actual) == 0) return;
        if (!(Math.abs(expected - actual) <= delta)) failNotEquals(message, Double.valueOf(expected), Double.valueOf(actual));
    }

    public static void assertEquals(double expected, double actual, double delta) {
        assertEquals(null, expected, actual, delta);
    }

    public static void assertEquals(String message, float expected, float actual, float delta) {
        if (Float.compare(expected, actual) == 0) return;
        if (!(Math.abs(expected - actual) <= delta)) failNotEquals(message, Float.valueOf(expected), Float.valueOf(actual));
    }

    public static void assertEquals(float expected, float actual, float delta) {
        assertEquals(null, expected, actual, delta);
    }

    public static void assertEquals(String message, long expected, long actual) {
        assertEquals(message, Long.valueOf(expected), Long.valueOf(actual));
    }

    public static void assertEquals(long expected, long actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertEquals(String message, boolean expected, boolean actual) {
        assertEquals(message, Boolean.valueOf(expected), Boolean.valueOf(actual));
    }

    public static void assertEquals(boolean expected, boolean actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertEquals(String message, byte expected, byte actual) {
        assertEquals(message, Byte.valueOf(expected), Byte.valueOf(actual));
    }

    public static void assertEquals(byte expected, byte actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertEquals(String message, char expected, char actual) {
        assertEquals(message, Character.valueOf(expected), Character.valueOf(actual));
    }

    public static void assertEquals(char expected, char actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertEquals(String message, short expected, short actual) {
        assertEquals(message, Short.valueOf(expected), Short.valueOf(actual));
    }

    public static void assertEquals(short expected, short actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertEquals(String message, int expected, int actual) {
        assertEquals(message, Integer.valueOf(expected), Integer.valueOf(actual));
    }

    public static void assertEquals(int expected, int actual) {
        assertEquals(null, expected, actual);
    }

    public static void assertNotNull(Object object) {
        assertNotNull(null, object);
    }

    public static void assertNotNull(String message, Object object) {
        assertTrue(message, object != null);
    }

    /** Without a message of the caller's, the failure says what the object was. */
    public static void assertNull(Object object) {
        if (object != null) assertNull("Expected: <null> but was: " + object, object);
    }

    public static void assertNull(String message, Object object) {
        assertTrue(message, object == null);
    }

    public static void assertSame(String message, Object expected, Object actual) {
        if (expected != actual) failNotSame(message, expected, actual);
    }

    public static void assertSame(Object expected, Object actual) {
        assertSame(null, expected, actual);
    }

    public static void assertNotSame(String message, Object expected, Object actual) {
        if (expected == actual) failSame(message);
    }

    public static void assertNotSame(Object expected, Object actual) {
        assertNotSame(null, expected, actual);
    }

    public static void failSame(String message) {
        fail(prefixed(message) + "expected not same");
    }

    public static void failNotSame(String message, Object expected, Object actual) {
        fail(prefixed(message) + "expected same:<" + expected + "> was not:<" + actual + ">");
    }

    public static void failNotEquals(String message, Object expected, Object actual) {
        fail(format(message, expected, actual));
    }

    /** "expected:<a> but was:<b>", after the message and a space where there is a message. */
    public static String format(String message, Object expected, Object actual) {
        return (message != null && !message.isEmpty() ? message + " " : "") + "expected:<" + expected + "> but was:<" + actual + ">";
    }

    private static boolean same(Object expected, Object actual) {
        return expected == null ? actual == null : expected.equals(actual);
    }

    /** failSame's and failNotSame's: an empty message is followed by its space too, as Android's are. */
    private static String prefixed(String message) {
        return message != null ? message + " " : "";
    }
}
