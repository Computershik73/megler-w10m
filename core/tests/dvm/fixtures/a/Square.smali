# A subclass that overrides and calls what it overrides.
.class public final Lt/Square;
.super Lt/Rect;

.method public constructor <init>(I)V
    .registers 3
    const-string v0, "square"
    invoke-direct {p0, v0, p1, p1}, Lt/Rect;-><init>(Ljava/lang/String;II)V
    return-void
.end method

# The superclass's area, and a thousand to tell that this one ran.
.method public area()I
    .registers 2
    invoke-super {p0}, Lt/Rect;->area()I
    move-result v0
    add-int/lit16 v0, v0, 0x3e8
    return v0
.end method

.method public kind()Ljava/lang/String;
    .registers 3
    const-string v0, "square:"
    invoke-super {p0}, Lt/Rect;->kind()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# The field is Shape's, named here through this class.
.method public nameViaSquare()Ljava/lang/String;
    .registers 2
    iget-object v0, p0, Lt/Square;->name:Ljava/lang/String;
    return-object v0
.end method
