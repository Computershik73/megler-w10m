# A class whose static initialiser takes a while and counts that it ran, for two threads to ask for at once.
.class public final Lt/Slow;
.super Ljava/lang/Object;

.field public static value:I

.method static constructor <clinit>()V
    .registers 2
    const v0, 0x30d40
    invoke-static {v0}, Lt/Arith;->sumTo(I)I
    move-result v0
    sput v0, Lt/Slow;->value:I
    invoke-static {}, Lt/Sync;->sbump()I
    return-void
.end method

.method public static get()I
    .registers 1
    sget v0, Lt/Slow;->value:I
    return v0
.end method
