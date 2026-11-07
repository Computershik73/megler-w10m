package de.robv.android.xposed;

import java.lang.reflect.Constructor;
import java.lang.reflect.Member;
import java.lang.reflect.Method;

/**
 * XposedBridge's native methods with the names and signatures Aliucord's Aliuhook (github.com/Aliucord/hook) gives them,
 * which are what binds them to the runtime's own (core/src/dvm/mg_dvm_hook.c): for the runtime's tests (hooks.Hooks).
 * Nothing else of Aliuhook's class is here; the tests' own Hook stands for its HookInfo.
 */
public final class XposedBridge {
    private XposedBridge() {}

    public static native Method hook0(Object context, Member original, Method callback);

    public static native boolean unhook0(Member target);

    public static native boolean deoptimize0(Member target);

    public static native boolean makeClassInheritable0(Class<?> target);

    public static native Object allocateInstance0(Class<?> clazz);

    public static native boolean invokeConstructor0(Object instance, Constructor<?> constructor, Object[] args);

    public static native boolean isHooked0(Member target);

    public static native boolean disableProfileSaver();

    public static native boolean disableHiddenApiRestrictions();
}
