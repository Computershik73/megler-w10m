# An abstract class that implements HasArea without declaring area(): calling area() on a Shape is a call
# of a method the class only inherits from its interface.
.class public abstract Lt/Shape;
.super Ljava/lang/Object;
.implements Lt/HasArea;

.field private static count:I
.field protected final name:Ljava/lang/String;

.method protected constructor <init>(Ljava/lang/String;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lt/Shape;->name:Ljava/lang/String;
    sget v0, Lt/Shape;->count:I
    add-int/lit8 v0, v0, 0x1
    sput v0, Lt/Shape;->count:I
    return-void
.end method

.method public static count()I
    .registers 1
    sget v0, Lt/Shape;->count:I
    return v0
.end method

.method public name()Ljava/lang/String;
    .registers 2
    iget-object v0, p0, Lt/Shape;->name:Ljava/lang/String;
    return-object v0
.end method

.method public twice()I
    .registers 2
    invoke-virtual {p0}, Lt/Shape;->area()I
    move-result v0
    mul-int/lit8 v0, v0, 0x2
    return v0
.end method

.method public kind()Ljava/lang/String;
    .registers 2
    const-string v0, "shape"
    return-object v0
.end method

# A private method: Rect has one of the same name, which must not be taken for an override.
.method private secret()I
    .registers 2
    const/4 v0, 0x7
    return v0
.end method

.method public viaPrivate()I
    .registers 2
    invoke-direct {p0}, Lt/Shape;->secret()I
    move-result v0
    return v0
.end method
