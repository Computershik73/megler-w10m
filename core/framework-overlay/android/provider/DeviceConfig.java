package android.provider;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * Megler: Android's store of the system's configuration flags (API 29, a system API), which the original lacked. Android
 * keeps them in the settings provider, written by the system's own apps -- Google Play services' platform configurator
 * hands its Phenotype flags over to them -- and read by the system's parts. A Megler phone has no such parts and nothing
 * here reads a flag: the store is the process's own, in memory, so what a hosted Play services writes it reads back, and
 * every namespace starts empty, as on a phone no server has configured yet. Without the class Play services' configurator
 * ended with NoClassDefFoundError at each start (Play Store, Aliucord, Jetpack Joyride, 0.1.0.690).
 */
public final class DeviceConfig {
    public static final String NAMESPACE_ACTIVITY_MANAGER = "activity_manager";
    public static final String NAMESPACE_ADSERVICES = "adservices";
    public static final String NAMESPACE_APPSEARCH = "appsearch";
    public static final String NAMESPACE_APP_COMPAT = "app_compat";
    public static final String NAMESPACE_APP_HIBERNATION = "app_hibernation";
    public static final String NAMESPACE_ATTENTION_MANAGER_SERVICE = "attention_manager_service";
    public static final String NAMESPACE_AUTOFILL = "autofill";
    public static final String NAMESPACE_BATTERY_SAVER = "battery_saver";
    public static final String NAMESPACE_BIOMETRICS = "biometrics";
    public static final String NAMESPACE_BLOBSTORE = "blobstore";
    public static final String NAMESPACE_BLUETOOTH = "bluetooth";
    public static final String NAMESPACE_CLIPBOARD = "clipboard";
    public static final String NAMESPACE_CONNECTIVITY = "connectivity";
    public static final String NAMESPACE_CONTENT_CAPTURE = "content_capture";
    public static final String NAMESPACE_DEX_BOOT = "dex_boot";
    public static final String NAMESPACE_DISPLAY_MANAGER = "display_manager";
    public static final String NAMESPACE_GAME_DRIVER = "game_driver";
    public static final String NAMESPACE_HEALTH_FITNESS = "health_fitness";
    public static final String NAMESPACE_INPUT_NATIVE_BOOT = "input_native_boot";
    public static final String NAMESPACE_INTELLIGENCE_ATTENTION = "intelligence_attention";
    public static final String NAMESPACE_JOB_SCHEDULER = "jobscheduler";
    public static final String NAMESPACE_LMKD_NATIVE = "lmkd_native";
    public static final String NAMESPACE_LOCATION = "location";
    public static final String NAMESPACE_MEDIA = "media";
    public static final String NAMESPACE_MEDIA_NATIVE = "media_native";
    public static final String NAMESPACE_MGLRU_NATIVE = "mglru_native";
    public static final String NAMESPACE_NETD_NATIVE = "netd_native";
    public static final String NAMESPACE_NNAPI_NATIVE = "nnapi_native";
    public static final String NAMESPACE_ON_DEVICE_PERSONALIZATION = "on_device_personalization";
    public static final String NAMESPACE_OTA = "ota";
    public static final String NAMESPACE_PACKAGE_MANAGER_SERVICE = "package_manager_service";
    public static final String NAMESPACE_PERMISSIONS = "permissions";
    public static final String NAMESPACE_PRIVACY = "privacy";
    public static final String NAMESPACE_PROFCOLLECT_NATIVE_BOOT = "profcollect_native_boot";
    public static final String NAMESPACE_REBOOT_READINESS = "reboot_readiness";
    public static final String NAMESPACE_ROLLBACK = "rollback";
    public static final String NAMESPACE_ROLLBACK_BOOT = "rollback_boot";
    public static final String NAMESPACE_RUNTIME = "runtime";
    public static final String NAMESPACE_RUNTIME_NATIVE = "runtime_native";
    public static final String NAMESPACE_RUNTIME_NATIVE_BOOT = "runtime_native_boot";
    public static final String NAMESPACE_SCHEDULER = "scheduler";
    public static final String NAMESPACE_SDK_SANDBOX = "sdk_sandbox";
    public static final String NAMESPACE_STATSD_JAVA = "statsd_java";
    public static final String NAMESPACE_STATSD_JAVA_BOOT = "statsd_java_boot";
    public static final String NAMESPACE_STATSD_NATIVE = "statsd_native";
    public static final String NAMESPACE_STATSD_NATIVE_BOOT = "statsd_native_boot";
    public static final String NAMESPACE_STORAGE_NATIVE_BOOT = "storage_native_boot";
    public static final String NAMESPACE_SURFACE_FLINGER_NATIVE_BOOT = "surface_flinger_native_boot";
    public static final String NAMESPACE_SWCODEC_NATIVE = "swcodec_native";
    public static final String NAMESPACE_SYSTEMUI = "systemui";
    public static final String NAMESPACE_SYSTEM_TIME = "system_time";
    public static final String NAMESPACE_TELEPHONY = "telephony";
    public static final String NAMESPACE_TETHERING = "tethering";
    public static final String NAMESPACE_TEXTCLASSIFIER = "textclassifier";
    public static final String NAMESPACE_UWB = "uwb";
    public static final String NAMESPACE_WINDOW_MANAGER_NATIVE_BOOT = "window_manager_native_boot";

    public static final int SYNC_DISABLED_MODE_NONE = 0;
    public static final int SYNC_DISABLED_MODE_PERSISTENT = 1;
    public static final int SYNC_DISABLED_MODE_UNTIL_REBOOT = 2;

    /* A namespace's flags by name; both maps under sLock. A listener is told on its executor, outside the lock. */
    private static final Object sLock = new Object();
    private static final HashMap<String, HashMap<String, String>> sFlags = new HashMap<>();
    private static final ArrayList<Listener> sListeners = new ArrayList<>();
    private static int sSyncDisabledMode = SYNC_DISABLED_MODE_NONE;

    private static final class Listener {
        final String namespace;
        final Executor executor;
        final OnPropertiesChangedListener listener;
        Listener(String namespace, Executor executor, OnPropertiesChangedListener listener) {
            this.namespace = namespace;
            this.executor = executor;
            this.listener = listener;
        }
    }

    private DeviceConfig() {}

    public static String getProperty(String namespace, String name) {
        if (namespace == null || name == null) throw new NullPointerException();
        synchronized (sLock) {
            HashMap<String, String> flags = sFlags.get(namespace);
            return flags == null ? null : flags.get(name);
        }
    }

    /** The namespace's flags of these names, or all of them when no name is given. */
    public static Properties getProperties(String namespace, String... names) {
        if (namespace == null) throw new NullPointerException();
        HashMap<String, String> found = new HashMap<>();
        synchronized (sLock) {
            HashMap<String, String> flags = sFlags.get(namespace);
            if (flags != null) {
                if (names == null || names.length == 0) found.putAll(flags);
                else
                    for (String name : names)
                        if (flags.containsKey(name)) found.put(name, flags.get(name));
            }
        }
        return new Properties(namespace, found);
    }

    public static String getString(String namespace, String name, String defaultValue) {
        String value = getProperty(namespace, name);
        return value != null ? value : defaultValue;
    }

    public static boolean getBoolean(String namespace, String name, boolean defaultValue) {
        String value = getProperty(namespace, name);
        return value != null ? Boolean.parseBoolean(value) : defaultValue;
    }

    public static int getInt(String namespace, String name, int defaultValue) {
        String value = getProperty(namespace, name);
        if (value == null) return defaultValue;
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static long getLong(String namespace, String name, long defaultValue) {
        String value = getProperty(namespace, name);
        if (value == null) return defaultValue;
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static float getFloat(String namespace, String name, float defaultValue) {
        String value = getProperty(namespace, name);
        if (value == null) return defaultValue;
        try {
            return Float.parseFloat(value);
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    /** Sets one flag (a null value takes it away); true, as Android answers a caller that may write. */
    public static boolean setProperty(String namespace, String name, String value, boolean makeDefault) {
        if (namespace == null || name == null) throw new NullPointerException();
        synchronized (sLock) {
            HashMap<String, String> flags = sFlags.get(namespace);
            if (value == null) {
                if (flags == null || !flags.containsKey(name)) return true;
                flags.remove(name);
            } else {
                if (flags == null) sFlags.put(namespace, flags = new HashMap<>());
                if (value.equals(flags.put(name, value))) return true;
            }
        }
        HashMap<String, String> changed = new HashMap<>();
        changed.put(name, value);
        tell(namespace, changed);
        return true;
    }

    /** The namespace's flags become these and no others, as Android replaces them in one step. */
    public static boolean setProperties(Properties properties) throws BadConfigException {
        if (properties == null) throw new NullPointerException();
        String namespace = properties.getNamespace();
        HashMap<String, String> changed = new HashMap<>();
        synchronized (sLock) {
            HashMap<String, String> old = sFlags.get(namespace);
            HashMap<String, String> now = new HashMap<>();
            for (String name : properties.getKeyset()) {
                String value = properties.getString(name, null);
                if (value == null) continue;
                now.put(name, value);
                if (old == null || !value.equals(old.get(name))) changed.put(name, value);
            }
            if (old != null)
                for (String name : old.keySet())
                    if (!now.containsKey(name)) changed.put(name, null);
            sFlags.put(namespace, now);
        }
        if (!changed.isEmpty()) tell(namespace, changed);
        return true;
    }

    public static boolean deleteProperty(String namespace, String name) {
        return setProperty(namespace, name, null, false);
    }

    /** No flag here has a default to go back to: the namespace's flags (every namespace's for null) are no more. */
    public static void resetToDefaults(int resetMode, String namespace) {
        ArrayList<String> emptied = new ArrayList<>();
        synchronized (sLock) {
            if (namespace == null) emptied.addAll(sFlags.keySet());
            else if (sFlags.containsKey(namespace)) emptied.add(namespace);
        }
        for (String each : emptied) {
            HashMap<String, String> changed = new HashMap<>();
            synchronized (sLock) {
                HashMap<String, String> old = sFlags.remove(each);
                if (old != null)
                    for (String name : old.keySet()) changed.put(name, null);
            }
            if (!changed.isEmpty()) tell(each, changed);
        }
    }

    public static void setSyncDisabledMode(int syncDisabledMode) {
        synchronized (sLock) {
            sSyncDisabledMode = syncDisabledMode;
        }
    }

    public static int getSyncDisabledMode() {
        synchronized (sLock) {
            return sSyncDisabledMode;
        }
    }

    public static List<String> getPublicNamespaces() {
        return Collections.emptyList();
    }

    public static Set<String> getAdbWritableNamespaces() {
        return Collections.emptySet();
    }

    public static void addOnPropertiesChangedListener(String namespace, Executor executor, OnPropertiesChangedListener onPropertiesChangedListener) {
        if (namespace == null || executor == null || onPropertiesChangedListener == null) throw new NullPointerException();
        synchronized (sLock) {
            for (int i = sListeners.size() - 1; i >= 0; i--)
                if (sListeners.get(i).listener == onPropertiesChangedListener) sListeners.remove(i);
            sListeners.add(new Listener(namespace, executor, onPropertiesChangedListener));
        }
    }

    public static void removeOnPropertiesChangedListener(OnPropertiesChangedListener onPropertiesChangedListener) {
        if (onPropertiesChangedListener == null) throw new NullPointerException();
        synchronized (sLock) {
            for (int i = sListeners.size() - 1; i >= 0; i--)
                if (sListeners.get(i).listener == onPropertiesChangedListener) sListeners.remove(i);
        }
    }

    /* The namespace's listeners hear of what changed (a flag taken away has a null value), each on its executor. */
    private static void tell(String namespace, HashMap<String, String> changed) {
        ArrayList<Listener> told = new ArrayList<>();
        synchronized (sLock) {
            for (Listener each : sListeners)
                if (each.namespace.equals(namespace)) told.add(each);
        }
        for (Listener each : told) {
            Properties properties = new Properties(namespace, changed);
            OnPropertiesChangedListener listener = each.listener;
            each.executor.execute(() -> listener.onPropertiesChanged(properties));
        }
    }

    public interface OnPropertiesChangedListener {
        void onPropertiesChanged(Properties properties);
    }

    /** What Android throws for a set of flags it refuses; nothing is refused here. */
    public static class BadConfigException extends Exception {
        public BadConfigException() {}
    }

    /** Flags of one namespace, as they were when they were read. */
    public static class Properties {
        private final String mNamespace;
        private final HashMap<String, String> mMap = new HashMap<>();

        public Properties(String namespace, Map<String, String> keyValueMap) {
            if (namespace == null) throw new NullPointerException();
            mNamespace = namespace;
            if (keyValueMap != null) mMap.putAll(keyValueMap);
        }

        public String getNamespace() {
            return mNamespace;
        }

        public Set<String> getKeyset() {
            return new HashSet<>(mMap.keySet());
        }

        public String getString(String name, String defaultValue) {
            if (name == null) throw new NullPointerException();
            String value = mMap.get(name);
            return value != null ? value : defaultValue;
        }

        public boolean getBoolean(String name, boolean defaultValue) {
            if (name == null) throw new NullPointerException();
            String value = mMap.get(name);
            return value != null ? Boolean.parseBoolean(value) : defaultValue;
        }

        public int getInt(String name, int defaultValue) {
            if (name == null) throw new NullPointerException();
            String value = mMap.get(name);
            if (value == null) return defaultValue;
            try {
                return Integer.parseInt(value);
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public long getLong(String name, long defaultValue) {
            if (name == null) throw new NullPointerException();
            String value = mMap.get(name);
            if (value == null) return defaultValue;
            try {
                return Long.parseLong(value);
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public float getFloat(String name, float defaultValue) {
            if (name == null) throw new NullPointerException();
            String value = mMap.get(name);
            if (value == null) return defaultValue;
            try {
                return Float.parseFloat(value);
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public static final class Builder {
            private final String mNamespace;
            private final HashMap<String, String> mKeyValues = new HashMap<>();

            public Builder(String namespace) {
                if (namespace == null) throw new NullPointerException();
                mNamespace = namespace;
            }

            public Builder setString(String name, String value) {
                mKeyValues.put(name, value);
                return this;
            }

            public Builder setBoolean(String name, boolean value) {
                mKeyValues.put(name, Boolean.toString(value));
                return this;
            }

            public Builder setInt(String name, int value) {
                mKeyValues.put(name, Integer.toString(value));
                return this;
            }

            public Builder setLong(String name, long value) {
                mKeyValues.put(name, Long.toString(value));
                return this;
            }

            public Builder setFloat(String name, float value) {
                mKeyValues.put(name, Float.toString(value));
                return this;
            }

            public Properties build() {
                return new Properties(mNamespace, mKeyValues);
            }
        }
    }
}
