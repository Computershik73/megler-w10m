# java.lang.Enum: what every enum class extends.
.class public abstract Ljava/lang/Enum;
.super Ljava/lang/Object;
.implements Ljava/lang/Comparable;
.implements Ljava/io/Serializable;

.field private final name:Ljava/lang/String;
.field private final ordinal:I

.method protected constructor <init>(Ljava/lang/String;I)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Ljava/lang/Enum;->name:Ljava/lang/String;
    iput p2, p0, Ljava/lang/Enum;->ordinal:I
    return-void
.end method

.method public final name()Ljava/lang/String;
    .registers 2
    iget-object v0, p0, Ljava/lang/Enum;->name:Ljava/lang/String;
    return-object v0
.end method

.method public final ordinal()I
    .registers 2
    iget v0, p0, Ljava/lang/Enum;->ordinal:I
    return v0
.end method

.method public toString()Ljava/lang/String;
    .registers 2
    iget-object v0, p0, Ljava/lang/Enum;->name:Ljava/lang/String;
    return-object v0
.end method

.method public final equals(Ljava/lang/Object;)Z
    .registers 3
    if-ne p0, p1, :differ
    const/4 v0, 0x1
    return v0
    :differ
    const/4 v0, 0x0
    return v0
.end method

.method public final hashCode()I
    .registers 2
    invoke-super {p0}, Ljava/lang/Object;->hashCode()I
    move-result v0
    return v0
.end method

.method public final compareTo(Ljava/lang/Enum;)I
    .registers 4
    iget v0, p0, Ljava/lang/Enum;->ordinal:I
    iget v1, p1, Ljava/lang/Enum;->ordinal:I
    sub-int/2addr v0, v1
    return v0
.end method

.method public bridge synthetic compareTo(Ljava/lang/Object;)I
    .registers 3
    check-cast p1, Ljava/lang/Enum;
    invoke-virtual {p0, p1}, Ljava/lang/Enum;->compareTo(Ljava/lang/Enum;)I
    move-result v0
    return v0
.end method

.method public static native valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;
.end method
