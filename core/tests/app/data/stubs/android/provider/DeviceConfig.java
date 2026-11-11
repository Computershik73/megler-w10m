package android.provider;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step device-config calls of Android's system API. */
public final class DeviceConfig {
    private DeviceConfig() {}
    public static String getProperty(String namespace, String name) { return null; }
    public static Properties getProperties(String namespace, String... names) { return null; }
    public static String getString(String namespace, String name, String defaultValue) { return null; }
    public static boolean getBoolean(String namespace, String name, boolean defaultValue) { return false; }
    public static int getInt(String namespace, String name, int defaultValue) { return 0; }
    public static boolean setProperty(String namespace, String name, String value, boolean makeDefault) { return false; }
    public static boolean setProperties(Properties properties) throws BadConfigException { return false; }
    public static boolean deleteProperty(String namespace, String name) { return false; }
    public static void addOnPropertiesChangedListener(String namespace, java.util.concurrent.Executor executor, OnPropertiesChangedListener listener) {}
    public static void removeOnPropertiesChangedListener(OnPropertiesChangedListener listener) {}
    public interface OnPropertiesChangedListener { void onPropertiesChanged(Properties properties); }
    public static class BadConfigException extends Exception {}
    public static class Properties {
        public String getNamespace() { return null; }
        public java.util.Set<String> getKeyset() { return null; }
        public String getString(String name, String defaultValue) { return null; }
        public long getLong(String name, long defaultValue) { return 0; }
        public static final class Builder {
            public Builder(String namespace) {}
            public Builder setString(String name, String value) { return this; }
            public Builder setBoolean(String name, boolean value) { return this; }
            public Builder setLong(String name, long value) { return this; }
            public Properties build() { return null; }
        }
    }
}
