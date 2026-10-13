# Its static initialiser needs the superclass's to have run.
.class public Lt/InitB;
.super Lt/InitA;

.field public static b:I

.method static constructor <clinit>()V
    .registers 1
    const-string v0, "B"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    sget v0, Lt/InitA;->a:I
    add-int/lit8 v0, v0, 0x1
    sput v0, Lt/InitB;->b:I
    return-void
.end method

.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Lt/InitA;-><init>()V
    const-string v0, "b"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    return-void
.end method

.method public static b()I
    .registers 1
    sget v0, Lt/InitB;->b:I
    return v0
.end method
