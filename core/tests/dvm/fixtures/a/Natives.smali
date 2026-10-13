# Native methods: implemented in C by the tests (hk_dvm_tests_runtime.c), called from bytecode and calling back.
.class public final Lt/Natives;
.super Ljava/lang/Object;

.field public base:I

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lt/Natives;->base:I
    return-void
.end method

.method public static native all(ZBCSIJFDLjava/lang/String;)J
.end method

.method public native plus(I)I
.end method

.method public static native retBool(I)Z
.end method

.method public static native retByte(I)B
.end method

.method public static native retChar(I)C
.end method

.method public static native retShort(I)S
.end method

.method public static native retFloat(F)F
.end method

.method public static native retDouble(D)D
.end method

.method public static native retObject(Ljava/lang/Object;)Ljava/lang/Object;
.end method

.method public static native thrower()V
.end method

.method public static native callback(I)I
.end method

.method public static native missing()V
.end method

.method public static native viaJni(IJ)J
.end method

.method public synchronized native locked()Z
.end method

# One argument of each kind, by range: true, -2, 'A', -300, 123456, 1 << 40, 1.5f, 2.5, "nine"
.method public static callAll()J
    .registers 11
    const/4 v0, 0x1
    const/4 v1, -0x2
    const/16 v2, 0x41
    const/16 v3, -0x12c
    const v4, 0x1e240
    const-wide v5, 0x10000000000L
    const/high16 v7, 0x3fc00000
    const-wide/high16 v8, 0x4004000000000000L
    const-string v10, "nine"
    invoke-static/range {v0 .. v10}, Lt/Natives;->all(ZBCSIJFDLjava/lang/String;)J
    move-result-wide v0
    return-wide v0
.end method

# What a native method returns as a small type reaches a register as that type: each is given a value with
# bits above its width set. true + (byte) 0x180 + (char) 0x1ffff + (short) 0x18000 = 1 - 128 + 65535 - 32768
.method public static rets()I
    .registers 3
    const/16 v0, 0x100
    invoke-static {v0}, Lt/Natives;->retBool(I)Z
    move-result v1
    const/16 v0, 0x180
    invoke-static {v0}, Lt/Natives;->retByte(I)B
    move-result v0
    add-int/2addr v1, v0
    const v0, 0x1ffff
    invoke-static {v0}, Lt/Natives;->retChar(I)C
    move-result v0
    add-int/2addr v1, v0
    const v0, 0x18000
    invoke-static {v0}, Lt/Natives;->retShort(I)S
    move-result v0
    add-int/2addr v1, v0
    return v1
.end method

.method public static floats(FD)D
    .registers 5
    invoke-static {p0}, Lt/Natives;->retFloat(F)F
    move-result v0
    float-to-double v0, v0
    invoke-static {p1, p2}, Lt/Natives;->retDouble(D)D
    move-result-wide p1
    add-double/2addr v0, p1
    return-wide v0
.end method

.method public static catchNative()I
    .registers 2
    :try_start
    invoke-static {}, Lt/Natives;->thrower()V
    :try_end
    .catch Ljava/lang/IllegalStateException; {:try_start .. :try_end} :handler
    const/4 v0, 0x0
    return v0
    :handler
    move-exception v0
    const/4 v0, 0x1
    return v0
.end method

# What the native callback(n) calls back: callback(n - 1) + 1. So callback(n) is n, by way of n trips
# from bytecode into C and back.
.method public static step(I)I
    .registers 2
    add-int/lit8 v0, p0, -0x1
    invoke-static {v0}, Lt/Natives;->callback(I)I
    move-result v0
    add-int/lit8 v0, v0, 0x1
    return v0
.end method

.method public static twice(I)I
    .registers 2
    mul-int/lit8 v0, p0, 0x2
    return v0
.end method
