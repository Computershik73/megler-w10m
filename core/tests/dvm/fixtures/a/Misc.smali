# What fits nowhere else: the long forms of goto, calls with many arguments, code that does not end.
.class public final Lt/Misc;
.super Ljava/lang/Object;

# n + 3, by way of goto/16, goto/32 and goto, forwards and backwards, and a nop
.method public static jumps(I)I
    .registers 1
    goto/16 :a
    :b
    add-int/lit8 p0, p0, 0x2
    goto/32 :c
    :a
    add-int/lit8 p0, p0, 0x1
    goto :b
    :c
    nop
    return p0
.end method

# a + 2b + 3c + ... + 10j
.method public static many(IIIIIIIIII)I
    .registers 12
    const/4 v0, 0x0
    add-int/2addr v0, p0
    mul-int/lit8 v1, p1, 0x2
    add-int/2addr v0, v1
    mul-int/lit8 v1, p2, 0x3
    add-int/2addr v0, v1
    mul-int/lit8 v1, p3, 0x4
    add-int/2addr v0, v1
    mul-int/lit8 v1, p4, 0x5
    add-int/2addr v0, v1
    mul-int/lit8 v1, p5, 0x6
    add-int/2addr v0, v1
    mul-int/lit8 v1, p6, 0x7
    add-int/2addr v0, v1
    mul-int/lit8 v1, p7, 0x8
    add-int/2addr v0, v1
    mul-int/lit8 v1, p8, 0x9
    add-int/2addr v0, v1
    mul-int/lit8 v1, p9, 0xa
    add-int/2addr v0, v1
    return v0
.end method

# many(1, 2, ..., 10) = 385, the ten arguments passed by range
.method public static callMany()I
    .registers 10
    const/4 v0, 0x1
    const/4 v1, 0x2
    const/4 v2, 0x3
    const/4 v3, 0x4
    const/4 v4, 0x5
    const/4 v5, 0x6
    const/4 v6, 0x7
    const/16 v7, 0x8
    const/16 v8, 0x9
    const/16 v9, 0xa
    invoke-static/range {v0 .. v9}, Lt/Misc;->many(IIIIIIIIII)I
    move-result v0
    return v0
.end method

.method public static spin()V
    .registers 1
    :loop
    nop
    goto :loop
.end method

# Catches everything and goes round again: only the step limit ends it, and nothing may catch that.
.method public static stubborn()V
    .registers 2
    :loop
    :try_start
    const/4 v0, 0x0
    invoke-virtual {v0}, Ljava/lang/Object;->hashCode()I
    :try_end
    .catchall {:try_start .. :try_end} :handler
    goto :loop
    :handler
    move-exception v1
    goto :loop
.end method

.method public static arrayClass()Ljava/lang/Class;
    .registers 1
    const-class v0, [I
    return-object v0
.end method

# null is an instance of nothing and can be cast to anything, even to a class that does not exist.
.method public static nulls()I
    .registers 3
    const/4 v0, 0x0
    instance-of v1, v0, Ljava/lang/String;
    check-cast v0, Lno/such/Class;
    instance-of v2, v0, Lno/such/Class;
    add-int/2addr v1, v2
    return v1
.end method

# A class that does not exist, used for real.
.method public static missing()Ljava/lang/Object;
    .registers 1
    new-instance v0, Lno/such/Class;
    return-object v0
.end method

.method public static missingMethod()V
    .registers 1
    invoke-static {}, Lt/Misc;->notThere()V
    return-void
.end method

.method public static missingField()I
    .registers 1
    sget v0, Lt/Misc;->notThere:I
    return v0
.end method

# A static method called as if it were virtual, and the other way round.
.method public static wrongKind()V
    .registers 1
    const-string v0, "x"
    invoke-virtual {v0}, Lt/Misc;->spin()V
    return-void
.end method
