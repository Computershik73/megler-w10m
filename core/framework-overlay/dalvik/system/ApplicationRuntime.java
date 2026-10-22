package dalvik.system;

import java.lang.reflect.Constructor;

/** Android 13's base APK optimization status. Megler runs DEX from its APK without AOT compilation. */
public final class ApplicationRuntime {
    private ApplicationRuntime() {}

    public static DexFile.OptimizationInfo getBaseApkOptimizationInfo() {
        try {
            Constructor<DexFile.OptimizationInfo> constructor =
                    DexFile.OptimizationInfo.class.getDeclaredConstructor(String.class, String.class);
            constructor.setAccessible(true);
            return constructor.newInstance("run-from-apk", "unknown");
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Cannot describe base APK optimization", e);
        }
    }
}
