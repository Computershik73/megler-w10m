package megler.test.data;

/**
 * The step trace-bytes: a byte[] given and one returned, for the second run's flag trace-calls-megler.test.data.Bytes,
 * which says a byte[]'s length and its first bytes (mg_dvm_interp.c, call_trace_bytes).
 */
public final class Bytes {
    private Bytes() {}

    public static byte[] echo(byte[] b) { return b.clone(); }
}
