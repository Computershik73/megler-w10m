package android.metrics;

/**
 * Android's reader of the system's metrics events (from the event log, which needs the system's permission). A Megler phone
 * logs none: a read finds nothing. GmsCore's metrics collection (tron) makes one when its service starts.
 */
public class MetricsReader {
    public MetricsReader() {}
    public void read(long horizonMs) {}
    public void reset() {}
    public void checkpoint() {}
    public boolean hasNext() { return false; }
    public LogMaker next() { return null; }
}
