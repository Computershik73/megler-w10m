# java.lang.Character, as a box only.
.class public final Ljava/lang/Character;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;

.field public static final MAX_VALUE:C = '￿'
.field private final value:C

.method public constructor <init>(C)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-char p1, p0, Ljava/lang/Character;->value:C
    return-void
.end method

.method public static valueOf(C)Ljava/lang/Character;
    .registers 2
    new-instance v0, Ljava/lang/Character;
    invoke-direct {v0, p0}, Ljava/lang/Character;-><init>(C)V
    return-object v0
.end method

.method public charValue()C
    .registers 2
    iget-char v0, p0, Ljava/lang/Character;->value:C
    return v0
.end method

.method public hashCode()I
    .registers 2
    iget-char v0, p0, Ljava/lang/Character;->value:C
    return v0
.end method

.method public equals(Ljava/lang/Object;)Z
    .registers 4
    instance-of v0, p1, Ljava/lang/Character;
    if-eqz v0, :no
    check-cast p1, Ljava/lang/Character;
    iget-char v0, p0, Ljava/lang/Character;->value:C
    iget-char v1, p1, Ljava/lang/Character;->value:C
    if-ne v0, v1, :no
    const/4 v0, 0x1
    return v0
    :no
    const/4 v0, 0x0
    return v0
.end method

.method public toString()Ljava/lang/String;
    .registers 2
    iget-char v0, p0, Ljava/lang/Character;->value:C
    invoke-static {v0}, Ljava/lang/String;->valueOf(C)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
