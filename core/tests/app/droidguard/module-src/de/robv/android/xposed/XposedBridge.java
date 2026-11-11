package de.robv.android.xposed;

import java.lang.reflect.Member;
import java.lang.reflect.Method;

/** The runtime's existing hook API, as used by core/tests/dvm/hooks. No native library is loaded. */
public final class XposedBridge {
    private XposedBridge() {}
    public static native Method hook0(Object context, Member original, Method callback);
}
