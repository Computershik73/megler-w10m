# Where the static initialisers of the Init* classes write that they ran.
.class public final Lt/InitLog;
.super Ljava/lang/Object;

.field public static log:Ljava/lang/StringBuilder;

.method static constructor <clinit>()V
    .registers 1
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    sput-object v0, Lt/InitLog;->log:Ljava/lang/StringBuilder;
    return-void
.end method

.method public static add(Ljava/lang/String;)V
    .registers 2
    sget-object v0, Lt/InitLog;->log:Ljava/lang/StringBuilder;
    invoke-virtual {v0, p0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    return-void
.end method

# What was written since the last time, which is then forgotten.
.method public static take()Ljava/lang/String;
    .registers 3
    sget-object v0, Lt/InitLog;->log:Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v1
    const/4 v2, 0x0
    invoke-virtual {v0, v2}, Ljava/lang/StringBuilder;->setLength(I)V
    return-object v1
.end method
