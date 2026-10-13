# The other half of InitC's circle.
.class public final Lt/InitD;
.super Ljava/lang/Object;

.field public static d:I

.method static constructor <clinit>()V
    .registers 1
    const-string v0, "D1"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    sget v0, Lt/InitC;->c:I
    add-int/lit8 v0, v0, 0x5
    sput v0, Lt/InitD;->d:I
    const-string v0, "D2"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    return-void
.end method
