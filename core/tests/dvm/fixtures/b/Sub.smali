# A class in the second file whose superclass, and its superclass, and their interface are in the first.
.class public final Lu/Sub;
.super Lt/Rect;

.method public constructor <init>()V
    .registers 3
    const/4 v0, 0x2
    const/4 v1, 0x5
    invoke-direct {p0, v0, v1}, Lt/Rect;-><init>(II)V
    return-void
.end method

.method public kind()Ljava/lang/String;
    .registers 2
    const-string v0, "sub"
    return-object v0
.end method
