# Not a Shape: it implements HasArea itself, and Comparable, and replaces what Object gives.
.class public final Lt/Blob;
.super Ljava/lang/Object;
.implements Lt/HasArea;
.implements Ljava/lang/Comparable;

.field private final size:I

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lt/Blob;->size:I
    return-void
.end method

.method public area()I
    .registers 2
    iget v0, p0, Lt/Blob;->size:I
    return v0
.end method

.method public compareTo(Ljava/lang/Object;)I
    .registers 4
    check-cast p1, Lt/Blob;
    iget v0, p0, Lt/Blob;->size:I
    iget v1, p1, Lt/Blob;->size:I
    sub-int/2addr v0, v1
    return v0
.end method

.method public hashCode()I
    .registers 2
    iget v0, p0, Lt/Blob;->size:I
    return v0
.end method

.method public equals(Ljava/lang/Object;)Z
    .registers 4
    instance-of v0, p1, Lt/Blob;
    if-eqz v0, :no
    check-cast p1, Lt/Blob;
    iget v0, p0, Lt/Blob;->size:I
    iget v1, p1, Lt/Blob;->size:I
    if-ne v0, v1, :no
    const/4 v0, 0x1
    return v0
    :no
    const/4 v0, 0x0
    return v0
.end method

.method public toString()Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    const-string v1, "blob "
    invoke-direct {v0, v1}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V
    iget v1, p0, Lt/Blob;->size:I
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
