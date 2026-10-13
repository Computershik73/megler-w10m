# packed-switch and sparse-switch: hits at each end, misses on each side, cases that fall into each other.
.class public final Lt/Switches;
.super Ljava/lang/Object;

# -1 -> 100, 0 -> 200, 1 -> 300, anything else -> -1
.method public static packed(I)I
    .registers 2
    packed-switch p0, :table
    const/4 v0, -0x1
    return v0
    :a
    const/16 v0, 0x64
    return v0
    :b
    const/16 v0, 0xc8
    return v0
    :c
    const/16 v0, 0x12c
    return v0
    :table
    .packed-switch -0x1
        :a
        :b
        :c
    .end packed-switch
.end method

.method public static sparse(I)I
    .registers 2
    sparse-switch p0, :table
    const/4 v0, 0x0
    return v0
    :neg
    const/4 v0, 0x1
    return v0
    :one
    const/4 v0, 0x2
    return v0
    :big
    const/4 v0, 0x3
    return v0
    :huge
    const/4 v0, 0x4
    return v0
    :table
    .sparse-switch
        -0x7fffffff -> :neg
        0x1 -> :one
        0x3e8 -> :big
        0x7fffffff -> :huge
    .end sparse-switch
.end method

# 3 -> 3+2+1, 2 -> 2+1, 1 -> 1, anything else -> 0
.method public static fall(I)I
    .registers 2
    const/4 v0, 0x0
    packed-switch p0, :table
    goto :done
    :three
    add-int/lit8 v0, v0, 0x3
    :two
    add-int/lit8 v0, v0, 0x2
    :one
    add-int/lit8 v0, v0, 0x1
    :done
    return v0
    :table
    .packed-switch 0x1
        :one
        :two
        :three
    .end packed-switch
.end method

# A switch inside a loop, its table before the code that uses it would be if the loop were unrolled:
# counts how many of 0..n-1 are 1 mod 3, by a sparse switch on i % 3.
.method public static counted(I)I
    .registers 4
    const/4 v0, 0x0
    const/4 v1, 0x0
    :loop
    if-ge v1, p0, :done
    rem-int/lit8 v2, v1, 0x3
    sparse-switch v2, :table
    goto :next
    :hit
    add-int/lit8 v0, v0, 0x1
    :next
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
    :table
    .sparse-switch
        0x1 -> :hit
    .end sparse-switch
.end method
