// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/**
 * The message of a ComparisonFailure: the two strings with what differs between them in brackets, after as much of what
 * they begin with alike and before as much of what they end with alike as `contextLength` allows, "..." where more of
 * either was left out -- "expected:<...lo wor[l]d> but was:<...lo wor[]d>". Strings that are equal, or a null, are given
 * whole (Assert.format).
 */
public class ComparisonCompactor {
    private static final String ELLIPSIS = "...";
    private static final String DELTA_START = "[";
    private static final String DELTA_END = "]";

    private final int context;
    private final String expected, actual;

    public ComparisonCompactor(int contextLength, String expected, String actual) {
        this.context = contextLength;
        this.expected = expected;
        this.actual = actual;
    }

    public String compact(String message) {
        if (expected == null || actual == null || expected.equals(actual)) return Assert.format(message, expected, actual);
        int shorter = Math.min(expected.length(), actual.length());
        // How long the two begin alike, and how long they then end alike, the end never reaching back into the beginning.
        int head = 0;
        while (head < shorter && expected.charAt(head) == actual.charAt(head)) head++;
        int tail = 0;
        while (tail < shorter - head && expected.charAt(expected.length() - 1 - tail) == actual.charAt(actual.length() - 1 - tail)) tail++;
        return Assert.format(message, shown(expected, head, tail), shown(actual, head, tail));
    }

    /** `s` with its differing middle bracketed, between the context of the common beginning and the common end. */
    private String shown(String s, int head, int tail) {
        StringBuilder b = new StringBuilder();
        if (head > context) b.append(ELLIPSIS);
        b.append(expected, Math.max(0, head - context), head);
        b.append(DELTA_START).append(s, head, s.length() - tail).append(DELTA_END);
        int end = expected.length() - tail;
        b.append(expected, end, Math.min(expected.length(), end + context));
        if (tail > context) b.append(ELLIPSIS);
        return b.toString();
    }
}
