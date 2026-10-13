# java.lang.Object, as far as bytecode needs it with no libcore.
# What is declared native here is in core/src/dvm/hk_dvm_boot.c.
.class public Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    return-void
.end method

.method public final native getClass()Ljava/lang/Class;
.end method

.method public native hashCode()I
.end method

.method public equals(Ljava/lang/Object;)Z
    .registers 3
    if-ne p0, p1, :differ
    const/4 v0, 0x1
    return v0
    :differ
    const/4 v0, 0x0
    return v0
.end method

.method public native toString()Ljava/lang/String;
.end method

.method protected native clone()Ljava/lang/Object;
.end method

.method public final native notify()V
.end method

.method public final native notifyAll()V
.end method

.method public final native wait()V
.end method

.method public final native wait(J)V
.end method

.method protected finalize()V
    .registers 1
    return-void
.end method
