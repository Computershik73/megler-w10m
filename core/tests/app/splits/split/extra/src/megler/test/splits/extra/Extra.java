package megler.test.splits.extra;

/** Code of the feature split "extra" alone: the base has none of it, and finds it by name. */
public final class Extra {
    private Extra() {}

    public static String hello() { return "hello from the extra split"; }
}
