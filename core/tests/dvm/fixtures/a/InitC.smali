# Two classes whose static initialisers need each other. Whichever is touched first starts, reaches the
# other, and the other sees the first half-made (its field still zero), as Java says it does.
.class public final Lt/InitC;
.super Ljava/lang/Object;

.field public static c:I

.method static constructor <clinit>()V
    .registers 1
    const-string v0, "C1"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    sget v0, Lt/InitD;->d:I
    add-int/lit8 v0, v0, 0x1
    sput v0, Lt/InitC;->c:I
    const-string v0, "C2"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    return-void
.end method

.method public static c()I
    .registers 1
    sget v0, Lt/InitC;->c:I
    return v0
.end method
