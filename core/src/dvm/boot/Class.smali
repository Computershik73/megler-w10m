# java.lang.Class. An object of it is made by the runtime for each class (mg_dvm_mirror), never by bytecode.
.class public final Ljava/lang/Class;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;

.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static native forName(Ljava/lang/String;)Ljava/lang/Class;
.end method

.method public native getName()Ljava/lang/String;
.end method

.method public native getSimpleName()Ljava/lang/String;
.end method

.method public native toString()Ljava/lang/String;
.end method

.method public native isInterface()Z
.end method

.method public native isArray()Z
.end method

.method public native isPrimitive()Z
.end method

.method public native getModifiers()I
.end method

.method public native getSuperclass()Ljava/lang/Class;
.end method

.method public native getComponentType()Ljava/lang/Class;
.end method

.method public native isInstance(Ljava/lang/Object;)Z
.end method

.method public native isAssignableFrom(Ljava/lang/Class;)Z
.end method

# What a class compiled with assertions asks in its static initialiser. They are off, as on Android.
.method public desiredAssertionStatus()Z
    .registers 2
    const/4 v0, 0x0
    return v0
.end method
