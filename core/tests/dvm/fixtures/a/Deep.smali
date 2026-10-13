# Recursion: deep, and without end.
.class public final Lt/Deep;
.super Ljava/lang/Object;

# n, by n calls each inside the last
.method public static down(I)I
    .registers 2
    if-nez p0, :more
    const/4 v0, 0x0
    return v0
    :more
    add-int/lit8 v0, p0, -0x1
    invoke-static {v0}, Lt/Deep;->down(I)I
    move-result v0
    add-int/lit8 v0, v0, 0x1
    return v0
.end method

.method public static forever(I)I
    .registers 2
    add-int/lit8 v0, p0, 0x1
    invoke-static {v0}, Lt/Deep;->forever(I)I
    move-result v0
    return v0
.end method

# The error is an exception like any other: caught here, at the bottom of everything it unwound.
.method public static caught()I
    .registers 2
    :try_start
    const/4 v0, 0x0
    invoke-static {v0}, Lt/Deep;->forever(I)I
    :try_end
    .catch Ljava/lang/StackOverflowError; {:try_start .. :try_end} :handler
    const/4 v0, 0x0
    return v0
    :handler
    move-exception v0
    const/4 v0, 0x1
    return v0
.end method
