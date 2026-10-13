# Calls on the shapes, each through the instruction under test.
.class public final Lt/Shapes;
.super Ljava/lang/Object;

# invoke-interface on whatever the array holds
.method public static total([Lt/HasArea;)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-object v3, p0, v1
    invoke-interface {v3}, Lt/HasArea;->area()I
    move-result v3
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# invoke-virtual through the abstract class
.method public static kind(Lt/Shape;)Ljava/lang/String;
    .registers 2
    invoke-virtual {p0}, Lt/Shape;->kind()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# invoke-interface of a default method
.method public static describe(Lt/HasArea;)Ljava/lang/String;
    .registers 2
    invoke-interface {p0}, Lt/HasArea;->describe()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# invoke-virtual of a default method, named through a class that only inherits it
.method public static describeRect(Lt/Rect;)Ljava/lang/String;
    .registers 2
    invoke-virtual {p0}, Lt/Rect;->describe()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# What an object is an instance of, one bit each: HasArea 1, Shape 2, Rect 4, Square 8, Comparable 16, String 32.
.method public static what(Ljava/lang/Object;)I
    .registers 3
    const/4 v0, 0x0
    instance-of v1, p0, Lt/HasArea;
    or-int/2addr v0, v1
    instance-of v1, p0, Lt/Shape;
    shl-int/lit8 v1, v1, 0x1
    or-int/2addr v0, v1
    instance-of v1, p0, Lt/Rect;
    shl-int/lit8 v1, v1, 0x2
    or-int/2addr v0, v1
    instance-of v1, p0, Lt/Square;
    shl-int/lit8 v1, v1, 0x3
    or-int/2addr v0, v1
    instance-of v1, p0, Ljava/lang/Comparable;
    shl-int/lit8 v1, v1, 0x4
    or-int/2addr v0, v1
    instance-of v1, p0, Ljava/lang/String;
    shl-int/lit8 v1, v1, 0x5
    or-int/2addr v0, v1
    return v0
.end method

# A static method and a constant, each named through a class that inherits it.
.method public static inherited()I
    .registers 2
    invoke-static {}, Lt/Rect;->count()I
    move-result v0
    mul-int/lit16 v0, v0, 0x3e8
    sget v1, Lt/Rect;->UNIT:I
    add-int/2addr v0, v1
    return v0
.end method

# Comparable.compareTo through the interface, on two Blobs.
.method public static compare(Ljava/lang/Comparable;Ljava/lang/Object;)I
    .registers 3
    invoke-interface {p0, p1}, Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I
    move-result v0
    return v0
.end method

# String.valueOf(Object), which calls the object's own toString().
.method public static text(Ljava/lang/Object;)Ljava/lang/String;
    .registers 2
    invoke-static {p0}, Ljava/lang/String;->valueOf(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static className(Ljava/lang/Object;)Ljava/lang/String;
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Object;->getClass()Ljava/lang/Class;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Class;->getName()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static superName(Ljava/lang/Object;)Ljava/lang/String;
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Object;->getClass()Ljava/lang/Class;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Class;->getSuperclass()Ljava/lang/Class;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Class;->getName()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# An abstract class cannot be made.
.method public static makeAbstract()Ljava/lang/Object;
    .registers 1
    new-instance v0, Lt/Shape;
    return-object v0
.end method
