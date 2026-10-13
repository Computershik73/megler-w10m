# A concrete Shape. Its fields come after the superclass's in the object.
.class public Lt/Rect;
.super Lt/Shape;

.field protected final w:I
.field protected final h:I

.method public constructor <init>(II)V
    .registers 4
    const-string v0, "rect"
    invoke-direct {p0, v0, p1, p2}, Lt/Rect;-><init>(Ljava/lang/String;II)V
    return-void
.end method

.method protected constructor <init>(Ljava/lang/String;II)V
    .registers 4
    invoke-direct {p0, p1}, Lt/Shape;-><init>(Ljava/lang/String;)V
    iput p2, p0, Lt/Rect;->w:I
    iput p3, p0, Lt/Rect;->h:I
    return-void
.end method

.method public area()I
    .registers 3
    iget v0, p0, Lt/Rect;->w:I
    iget v1, p0, Lt/Rect;->h:I
    mul-int/2addr v0, v1
    return v0
.end method

.method public kind()Ljava/lang/String;
    .registers 2
    const-string v0, "rect"
    return-object v0
.end method

.method private secret()I
    .registers 2
    const/16 v0, 0x9
    return v0
.end method

.method public ownSecret()I
    .registers 2
    invoke-direct {p0}, Lt/Rect;->secret()I
    move-result v0
    return v0
.end method
