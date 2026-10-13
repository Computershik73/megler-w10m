# An interface with a constant, an abstract method, and a default method that calls it.
.class public interface abstract Lt/HasArea;
.super Ljava/lang/Object;

.field public static final UNIT:I = 0x7

.method public abstract area()I
.end method

.method public describe()Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "area "
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-interface {p0}, Lt/HasArea;->area()I
    move-result v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
