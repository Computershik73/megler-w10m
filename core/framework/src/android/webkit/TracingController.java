package android.webkit;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public abstract class TracingController {
    private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
    public TracingController() {}
    public static android.webkit.TracingController getInstance() { synchronized (TracingController.class) { if (sInstance == null) sInstance = new Meglet(); return sInstance; } }
    public abstract boolean isTracing();
    public abstract void start(android.webkit.TracingConfig p0);
    public abstract boolean stop(java.io.OutputStream p0, java.util.concurrent.Executor p1);
    static final class Meglet extends TracingController {
        public boolean isTracing() { return false; }
        public void start(android.webkit.TracingConfig p0) {}
        public boolean stop(java.io.OutputStream p0, java.util.concurrent.Executor p1) { return false; }
    }
    private static TracingController sInstance;
}
