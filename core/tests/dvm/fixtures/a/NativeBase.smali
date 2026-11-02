# A native method declared by a class and registered through its subclass (NativeChild), as Reanimated registers
# NativeProxyCommon's on NativeProxy: RegisterNatives finds it in the superclass, as ART does (mg_dvm_tests_jni.c).
.class public Lt/NativeBase;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public native installBindings()V
.end method
