# The instructions the interpreter does not implement. Each must be refused with an exception that says
# which, never run as something else. Having invoke-custom makes this a version 038 DEX file, with the
# call site and method handle tables that version adds.
.class public final Lt/Refuse;
.super Ljava/lang/Object;

.method public static polymorphic(Ljava/lang/Object;)V
    .registers 1
    invoke-polymorphic {p0}, Ljava/lang/invoke/MethodHandle;->invoke([Ljava/lang/Object;)Ljava/lang/Object;, ()V
    return-void
.end method

.method public static polymorphicRange(Ljava/lang/Object;)V
    .registers 1
    invoke-polymorphic/range {p0 .. p0}, Ljava/lang/invoke/MethodHandle;->invoke([Ljava/lang/Object;)Ljava/lang/Object;, ()V
    return-void
.end method

.method public static custom(I)V
    .registers 1
    invoke-custom {p0}, site("run", (I)V, "extra", 0x7)@Lt/Refuse;->bootstrap(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;
    return-void
.end method

.method public static customRange(I)V
    .registers 1
    invoke-custom/range {p0 .. p0}, site("run", (I)V, "extra", 0x7)@Lt/Refuse;->bootstrap(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;
    return-void
.end method

.method public static methodType()V
    .registers 1
    const-method-type v0, (I)V
    return-void
.end method

.method public static bootstrap(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;
    .registers 4
    const/4 v0, 0x0
    return-object v0
.end method

# The refusal is a Java exception, which Java code can catch.
.method public static caught()I
    .registers 2
    :try_start
    const/4 v0, 0x1
    invoke-static {v0}, Lt/Refuse;->custom(I)V
    :try_end
    .catch Ljava/lang/UnsupportedOperationException; {:try_start .. :try_end} :handler
    const/4 v0, 0x0
    return v0
    :handler
    move-exception v0
    const/4 v0, 0x1
    return v0
.end method

# Ordinary code in the same file runs as ever.
.method public static plain(I)I
    .registers 2
    add-int/lit8 v0, p0, 0x1
    return v0
.end method
