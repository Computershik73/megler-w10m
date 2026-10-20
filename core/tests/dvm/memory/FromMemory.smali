# The one class of m.dex, which the runtime's tests do not put on the class path: they read the file into a byte array
# and hand it to dalvik.system.InMemoryDexClassLoader, as a library that carries its own Java code does
# (mg_dvm_tests_libcore.c, test_in_memory_dex).
.class public Lt/FromMemory;
.super Ljava/lang/Object;

.method public static answer()I
    .registers 1
    const/16 v0, 0x2a
    return v0
.end method
