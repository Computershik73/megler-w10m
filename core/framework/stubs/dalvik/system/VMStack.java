package dalvik.system;

/** Compile-time shape of Android's VMStack, supplied by the Android root at runtime (the runtime's natives, dvm/mg_dvm_art.c). */
public final class VMStack {
    private VMStack() {}
    /** The class loader of the class whose method called the method that calls this. */
    public static native ClassLoader getCallingClassLoader();
}
