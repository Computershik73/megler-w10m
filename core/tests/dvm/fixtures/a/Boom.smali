# A class whose static initialiser divides by zero: unusable, and it must say so each time it is touched.
.class public final Lt/Boom;
.super Ljava/lang/Object;

.field public static x:I

.method static constructor <clinit>()V
    .registers 2
    const/4 v0, 0x1
    const/4 v1, 0x0
    div-int/2addr v0, v1
    sput v0, Lt/Boom;->x:I
    return-void
.end method

.method public static touch()I
    .registers 1
    sget v0, Lt/Boom;->x:I
    return v0
.end method
