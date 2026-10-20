package android.webkit;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public abstract class ServiceWorkerController {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public ServiceWorkerController() {}
    public static android.webkit.ServiceWorkerController getInstance() { synchronized (ServiceWorkerController.class) { if (sInstance == null) sInstance = new Megler(); return sInstance; } }
    public abstract android.webkit.ServiceWorkerWebSettings getServiceWorkerWebSettings();
    public abstract void setServiceWorkerClient(android.webkit.ServiceWorkerClient p0);
    static final class Megler extends ServiceWorkerController {
        private final ServiceWorkerWebSettings mSettings = new ServiceWorkerWebSettings.Megler();
        public android.webkit.ServiceWorkerWebSettings getServiceWorkerWebSettings() { return mSettings; }
        public void setServiceWorkerClient(android.webkit.ServiceWorkerClient p0) {}
    }
    private static ServiceWorkerController sInstance;
}
