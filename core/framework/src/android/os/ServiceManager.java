package android.os;

/**
 * Megler: Android's hidden ServiceManager, which apps reach by reflection for the system's binders -- Shizuku's and Sui's
 * SystemServiceHelper asks it for "package" to find out whether Sui is there. Megler's system services are objects in the
 * app's own process (Context.getSystemService), with no binders published anywhere: every name is a service that is not
 * there, as a service missing on a phone is. getService and checkService answer null, listServices nothing, and
 * getServiceOrThrow throws ServiceNotFoundException; services added are not kept.
 *
 * Morphe Manager ended at its start without the class (0.1.0.673): SystemServiceHelper found no ServiceManager, kept no
 * Method for getService, and Sui.init invoked that null Method.
 */
public final class ServiceManager {
    private ServiceManager() {}

    /** What getServiceOrThrow throws, as Android's does. */
    public static class ServiceNotFoundException extends Exception {
        public ServiceNotFoundException(String name) {
            super("No service published for: " + name);
        }
    }

    public static IBinder getService(String name) { return null; }

    public static IBinder getServiceOrThrow(String name) throws ServiceNotFoundException { throw new ServiceNotFoundException(name); }

    public static IBinder checkService(String name) { return null; }

    public static IBinder waitForService(String name) { return null; }

    public static IBinder waitForDeclaredService(String name) { return null; }

    public static boolean isDeclared(String name) { return false; }

    public static String[] getDeclaredInstances(String iface) { return new String[0]; }

    public static void addService(String name, IBinder service) {}

    public static void addService(String name, IBinder service, boolean allowIsolated) {}

    public static void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) {}

    public static String[] listServices() { return new String[0]; }
}
