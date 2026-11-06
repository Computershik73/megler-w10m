# What does and does not make a class initialise.
.class public final Lt/Init;
.super Ljava/lang/Object;

# Naming a class does not.
.method public static classOnly()Ljava/lang/Class;
    .registers 1
    const-class v0, Lt/InitB;
    return-object v0
.end method

# Nor does a check against it.
.method public static checkOnly(Ljava/lang/Object;)Z
    .registers 2
    instance-of v0, p0, Lt/InitB;
    return v0
.end method

# Reading InitA's field through InitB initialises InitA alone: the field is InitA's.
.method public static aViaB()I
    .registers 1
    sget v0, Lt/InitB;->a:I
    return v0
.end method

# Making an object does, the superclass first; then the constructors run, the superclass's first.
.method public static makeB()Ljava/lang/Object;
    .registers 1
    new-instance v0, Lt/InitB;
    invoke-direct {v0}, Lt/InitB;-><init>()V
    return-object v0
.end method

# A constructor InitF does not declare is InitA's, as ART resolves it: true when the object made is an InitF.
.method public static makeF()Z
    .registers 1
    new-instance v0, Lt/InitF;
    invoke-direct {v0}, Lt/InitF;-><init>()V
    instance-of v0, v0, Lt/InitF;
    return v0
.end method
