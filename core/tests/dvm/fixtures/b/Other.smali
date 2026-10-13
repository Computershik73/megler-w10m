# The second DEX file: classes that lean on the first one's.
.class public final Lu/Other;
.super Ljava/lang/Object;

# A static method of a class in the other file.
.method public static viaArith(II)I
    .registers 3
    invoke-static {p0, p1}, Lt/Arith;->sum(II)I
    move-result v0
    add-int/lit8 v0, v0, 0x64
    return v0
.end method

# The same literal as t.Strings.shared(): one object, whichever file names it.
.method public static shared()Ljava/lang/String;
    .registers 1
    const-string v0, "one string in two files"
    return-object v0
.end method

# t.Dup is defined in both files: the first definition is the one that exists.
.method public static dup()I
    .registers 1
    invoke-static {}, Lt/Dup;->which()I
    move-result v0
    return v0
.end method

.method public static makeSub()Lu/Sub;
    .registers 1
    new-instance v0, Lu/Sub;
    invoke-direct {v0}, Lu/Sub;-><init>()V
    return-object v0
.end method

# twice() is t.Shape's, area() t.Rect's, both reached through this file's class.
.method public static subTwice()I
    .registers 1
    invoke-static {}, Lu/Other;->makeSub()Lu/Sub;
    move-result-object v0
    invoke-virtual {v0}, Lu/Sub;->twice()I
    move-result v0
    return v0
.end method
