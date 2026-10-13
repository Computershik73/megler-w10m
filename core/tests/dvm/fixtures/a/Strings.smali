# Strings: constants and their identity, what d8 makes of concatenation, text outside ASCII, new String(...).
.class public final Lt/Strings;
.super Ljava/lang/Object;

.method public static shared()Ljava/lang/String;
    .registers 1
    const-string v0, "one string in two files"
    return-object v0
.end method

# Two uses of one literal are one object.
.method public static same()Z
    .registers 3
    const-string v0, "twice"
    const-string v1, "twice"
    if-ne v0, v1, :differ
    const/4 v2, 0x1
    return v2
    :differ
    const/4 v2, 0x0
    return v2
.end method

.method public static jumbo()Ljava/lang/String;
    .registers 1
    const-string/jumbo v0, "jumbo"
    return-object v0
.end method

# s + i + j + c + z + d + o, as d8 writes it: a StringBuilder and an append for each.
.method public static concat(Ljava/lang/String;IJCZDLjava/lang/Object;)Ljava/lang/String;
    .registers 11
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    invoke-virtual {v0, p0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0, p1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v0, p2, p3}, Ljava/lang/StringBuilder;->append(J)Ljava/lang/StringBuilder;
    invoke-virtual {v0, p4}, Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;
    invoke-virtual {v0, p5}, Ljava/lang/StringBuilder;->append(Z)Ljava/lang/StringBuilder;
    invoke-virtual {v0, p6, p7}, Ljava/lang/StringBuilder;->append(D)Ljava/lang/StringBuilder;
    move-result-object v1
    invoke-virtual {v1, p8}, Ljava/lang/StringBuilder;->append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# Past the sixteen characters a StringBuilder starts with: "0123456789" four times.
.method public static grown()Ljava/lang/String;
    .registers 4
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const/4 v1, 0x0
    :loop
    const/16 v2, 0x28
    if-ge v1, v2, :done
    rem-int/lit8 v2, v1, 0xa
    invoke-virtual {v0, v2}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# One character of two bytes in the file, one of three, a pair of surrogates, a zero, and a z.
.method public static unicode()Ljava/lang/String;
    .registers 1
    const-string v0, "é中😀\u0000z"
    return-object v0
.end method

# "hello".charAt(1) + 1000 * "hello".length()
.method public static chars()I
    .registers 3
    const-string v0, "hello"
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Ljava/lang/String;->charAt(I)C
    move-result v1
    invoke-virtual {v0}, Ljava/lang/String;->length()I
    move-result v2
    mul-int/lit16 v2, v2, 0x3e8
    add-int/2addr v1, v2
    return v1
.end method

.method public static hash()I
    .registers 1
    const-string v0, "hello"
    invoke-virtual {v0}, Ljava/lang/String;->hashCode()I
    move-result v0
    return v0
.end method

# new String(char[]): the object new-instance made is replaced by the constructor's result in every
# register that held it. The copy made before the constructor ran is what is returned.
.method public static fromChars()Ljava/lang/String;
    .registers 4
    new-instance v0, Ljava/lang/String;
    move-object v2, v0
    const/4 v1, 0x3
    new-array v1, v1, [C
    fill-array-data v1, :abc
    invoke-direct {v0, v1}, Ljava/lang/String;-><init>([C)V
    return-object v2
    :abc
    .array-data 2
        0x61 0x62 0x63
    .end array-data
.end method

# 1: new String("abc") equals "abc"   2: and is another object   4: and its intern() is the literal
.method public static identity()I
    .registers 5
    const-string v0, "abc"
    new-instance v1, Ljava/lang/String;
    invoke-direct {v1, v0}, Ljava/lang/String;-><init>(Ljava/lang/String;)V
    const/4 v2, 0x0
    invoke-virtual {v1, v0}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z
    move-result v3
    or-int/2addr v2, v3
    if-eq v1, v0, :same
    or-int/lit8 v2, v2, 0x2
    :same
    invoke-virtual {v1}, Ljava/lang/String;->intern()Ljava/lang/String;
    move-result-object v3
    if-ne v3, v0, :done
    or-int/lit8 v2, v2, 0x4
    :done
    return v2
.end method

# Through Comparable, which reaches String's bridge method: negative, zero, positive as -1, 0, 1 in three digits.
.method public static compare()I
    .registers 5
    const-string v0, "abc"
    const-string v1, "abd"
    invoke-interface {v0, v1}, Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I
    move-result v2
    invoke-static {v2}, Lt/Strings;->sign(I)I
    move-result v2
    mul-int/lit8 v2, v2, 0x64
    invoke-interface {v0, v0}, Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I
    move-result v3
    invoke-static {v3}, Lt/Strings;->sign(I)I
    move-result v3
    mul-int/lit8 v3, v3, 0xa
    add-int/2addr v2, v3
    invoke-interface {v1, v0}, Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I
    move-result v3
    invoke-static {v3}, Lt/Strings;->sign(I)I
    move-result v3
    add-int/2addr v2, v3
    return v2
.end method

.method private static sign(I)I
    .registers 2
    if-gez p0, :not_negative
    const/4 v0, -0x1
    return v0
    :not_negative
    if-eqz p0, :zero
    const/4 v0, 0x1
    return v0
    :zero
    const/4 v0, 0x0
    return v0
.end method

# A CharSequence that is a String and one that is a StringBuilder, through the interface.
.method public static lengths()I
    .registers 4
    const-string v0, "four"
    invoke-interface {v0}, Ljava/lang/CharSequence;->length()I
    move-result v1
    new-instance v2, Ljava/lang/StringBuilder;
    const-string v3, "sixsix"
    invoke-direct {v2, v3}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V
    invoke-interface {v2}, Ljava/lang/CharSequence;->length()I
    move-result v3
    mul-int/lit8 v1, v1, 0xa
    add-int/2addr v1, v3
    return v1
.end method
