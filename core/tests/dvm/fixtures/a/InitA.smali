# A superclass with a static initialiser.
.class public Lt/InitA;
.super Ljava/lang/Object;

.field public static a:I

.method static constructor <clinit>()V
    .registers 1
    const-string v0, "A"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    const/16 v0, 0xa
    sput v0, Lt/InitA;->a:I
    return-void
.end method

.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const-string v0, "a"
    invoke-static {v0}, Lt/InitLog;->add(Ljava/lang/String;)V
    return-void
.end method
