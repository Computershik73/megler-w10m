# Arrays of every element type: made, filled, read back with the right extension, and refused where Java refuses.
.class public final Lt/Arrays;
.super Ljava/lang/Object;

# The sum of the squares below n, through an int[].
.method public static squares(I)I
    .registers 5
    new-array v0, p0, [I
    const/4 v1, 0x0
    :fill
    if-ge v1, p0, :filled
    mul-int v2, v1, v1
    aput v2, v0, v1
    add-int/lit8 v1, v1, 0x1
    goto :fill
    :filled
    const/4 v1, 0x0
    const/4 v2, 0x0
    array-length v3, v0
    :sum
    if-ge v1, v3, :done
    aget p0, v0, v1
    add-int/2addr v2, p0
    add-int/lit8 v1, v1, 0x1
    goto :sum
    :done
    return v2
.end method

# One method for each small element type: what goes in comes out as that type holds it.
.method public static bools(I)I
    .registers 4
    const/4 v0, 0x2
    new-array v0, v0, [Z
    const/4 v1, 0x1
    aput-boolean p0, v0, v1
    aget-boolean v2, v0, v1
    return v2
.end method

.method public static bytes(I)I
    .registers 4
    const/4 v0, 0x2
    new-array v0, v0, [B
    const/4 v1, 0x1
    aput-byte p0, v0, v1
    aget-byte v2, v0, v1
    return v2
.end method

.method public static chars(I)I
    .registers 4
    const/4 v0, 0x2
    new-array v0, v0, [C
    const/4 v1, 0x1
    aput-char p0, v0, v1
    aget-char v2, v0, v1
    return v2
.end method

.method public static shorts(I)I
    .registers 4
    const/4 v0, 0x2
    new-array v0, v0, [S
    const/4 v1, 0x1
    aput-short p0, v0, v1
    aget-short v2, v0, v1
    return v2
.end method

.method public static longs(J)J
    .registers 6
    const/4 v0, 0x3
    new-array v0, v0, [J
    const/4 v1, 0x2
    aput-wide p0, v0, v1
    aget-wide v2, v0, v1
    return-wide v2
.end method

.method public static floats(F)F
    .registers 4
    const/4 v0, 0x2
    new-array v0, v0, [F
    const/4 v1, 0x1
    aput p0, v0, v1
    aget v2, v0, v1
    return v2
.end method

.method public static doubles(D)D
    .registers 6
    const/4 v0, 0x3
    new-array v0, v0, [D
    const/4 v1, 0x2
    aput-wide p0, v0, v1
    aget-wide v2, v0, v1
    return-wide v2
.end method

# fill-array-data at each element width, with three bytes (an odd count, so the data is padded).
# Returns bytes[2] + chars[1] + ints[0] + longs[2] + (long) (floats[1] * 4) + (long) (doubles[1] * 4).
.method public static filled()J
    .registers 10
    const/4 v0, 0x3
    new-array v1, v0, [B
    fill-array-data v1, :bytes
    new-array v2, v0, [C
    fill-array-data v2, :chars
    new-array v3, v0, [I
    fill-array-data v3, :ints
    new-array v4, v0, [J
    fill-array-data v4, :longs
    const/4 v5, 0x2
    aget-byte v6, v1, v5
    const/4 v7, 0x1
    aget-char v8, v2, v7
    add-int/2addr v6, v8
    const/4 v8, 0x0
    aget v8, v3, v8
    add-int/2addr v6, v8
    int-to-long v0, v6
    aget-wide v2, v4, v5
    add-long/2addr v0, v2
    new-array v4, v5, [F
    fill-array-data v4, :floats
    aget v6, v4, v7
    const/high16 v8, 0x40800000
    mul-float/2addr v6, v8
    float-to-long v2, v6
    add-long/2addr v0, v2
    new-array v4, v5, [D
    fill-array-data v4, :doubles
    aget-wide v2, v4, v7
    const-wide/high16 v8, 0x4010000000000000L
    mul-double/2addr v2, v8
    double-to-long v2, v2
    add-long/2addr v0, v2
    return-wide v0
    :bytes
    .array-data 1
        0x1t -0x2t 0x7ft
    .end array-data
    :chars
    .array-data 2
        0x41 0x1234 -0x1
    .end array-data
    :ints
    .array-data 4
        0x12345678 -0x1 0x7fffffff
    .end array-data
    :longs
    .array-data 8
        0x1L -0x1L 0x123456789abcdef0L
    .end array-data
    :floats
    .array-data 4
        1.5f 2.75f
    .end array-data
    :doubles
    .array-data 8
        1.5 -3.25
    .end array-data
.end method

# More data than the array holds.
.method public static overfill()V
    .registers 2
    const/4 v0, 0x2
    new-array v0, v0, [I
    fill-array-data v0, :three
    return-void
    :three
    .array-data 4
        0x1 0x2 0x3
    .end array-data
.end method

# filled-new-array of ints: a*100 + b*10 + c + 1000 * length
.method public static filledNew(III)I
    .registers 6
    filled-new-array {p0, p1, p2}, [I
    move-result-object v0
    const/4 v1, 0x0
    aget v2, v0, v1
    mul-int/lit8 v2, v2, 0x64
    const/4 v1, 0x1
    aget v1, v0, v1
    mul-int/lit8 v1, v1, 0xa
    add-int/2addr v2, v1
    const/4 v1, 0x2
    aget v1, v0, v1
    add-int/2addr v2, v1
    array-length v1, v0
    mul-int/lit16 v1, v1, 0x3e8
    add-int/2addr v2, v1
    return v2
.end method

# filled-new-array/range of objects: the length of the last of three strings
.method public static filledObjects()I
    .registers 4
    const-string v0, "a"
    const-string v1, "bb"
    const-string v2, "ccc"
    filled-new-array/range {v0 .. v2}, [Ljava/lang/String;
    move-result-object v3
    const/4 v0, 0x2
    aget-object v0, v3, v0
    invoke-virtual {v0}, Ljava/lang/String;->length()I
    move-result v0
    return v0
.end method

# An int[rows][cols] made row by row, each cell row * 10 + col; returns the sum of all of it.
.method public static grid(II)I
    .registers 8
    new-array v0, p0, [[I
    const/4 v1, 0x0
    :rows
    if-ge v1, p0, :made
    new-array v2, p1, [I
    aput-object v2, v0, v1
    const/4 v3, 0x0
    :cols
    if-ge v3, p1, :row_done
    mul-int/lit8 v4, v1, 0xa
    add-int/2addr v4, v3
    aput v4, v2, v3
    add-int/lit8 v3, v3, 0x1
    goto :cols
    :row_done
    add-int/lit8 v1, v1, 0x1
    goto :rows
    :made
    const/4 v5, 0x0
    const/4 v1, 0x0
    :sum_rows
    if-ge v1, p0, :done
    aget-object v2, v0, v1
    const/4 v3, 0x0
    :sum_cols
    array-length v4, v2
    if-ge v3, v4, :sum_row_done
    aget v4, v2, v3
    add-int/2addr v5, v4
    add-int/lit8 v3, v3, 0x1
    goto :sum_cols
    :sum_row_done
    add-int/lit8 v1, v1, 0x1
    goto :sum_rows
    :done
    return v5
.end method

# {10, 20, 30}[i]
.method public static at(I)I
    .registers 3
    const/4 v0, 0x3
    new-array v0, v0, [I
    fill-array-data v0, :data
    aget v0, v0, p0
    return v0
    :data
    .array-data 4
        0xa 0x14 0x1e
    .end array-data
.end method

.method public static nullLength()I
    .registers 1
    const/4 v0, 0x0
    array-length v0, v0
    return v0
.end method

.method public static nullRead()I
    .registers 2
    const/4 v0, 0x0
    const/4 v1, 0x0
    aget v0, v0, v1
    return v0
.end method

.method public static sized(I)I
    .registers 2
    new-array v0, p0, [J
    array-length v0, v0
    return v0
.end method

# A String[] takes a String and null, and refuses an Integer.
.method public static store()V
    .registers 4
    const/4 v0, 0x1
    new-array v1, v0, [Ljava/lang/String;
    const/4 v2, 0x0
    const-string v3, "fits"
    aput-object v3, v1, v2
    const/4 v3, 0x0
    aput-object v3, v1, v2
    invoke-static {v0}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v3
    aput-object v3, v1, v2
    return-void
.end method

# clone() of an array, and System.arraycopy over itself. Returns 1399: the original's [0] still 1, its [3]
# now 3 (three elements moved up by one), the clone's [0] the 99 stored into it.
.method public static copy()I
    .registers 7
    const/4 v0, 0x4
    new-array v1, v0, [I
    fill-array-data v1, :data
    invoke-virtual {v1}, [I->clone()Ljava/lang/Object;
    move-result-object v2
    check-cast v2, [I
    const/4 v3, 0x0
    const/16 v4, 0x63
    aput v4, v2, v3
    aget v5, v1, v3
    const/4 v4, 0x1
    const/4 v6, 0x3
    invoke-static {v1, v3, v1, v4, v6}, Ljava/lang/System;->arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V
    aget v4, v1, v6
    mul-int/lit8 v5, v5, 0xa
    add-int/2addr v5, v4
    aget v4, v2, v3
    mul-int/lit8 v5, v5, 0x64
    add-int/2addr v5, v4
    return v5
    :data
    .array-data 4
        0x1 0x2 0x3 0x4
    .end array-data
.end method

# What arrays are instances of, one bit each:
#   1  int[] is an Object            2  String[] is an Object[]       4  int[] is an Object[] (no)
#   8  int[][] is an Object[]       16  String[] is Cloneable        32  int[] is a long[] (no)
#  64  String[] is a CharSequence[] (yes: String is one)
.method public static kinds()I
    .registers 6
    const/4 v0, 0x1
    new-array v1, v0, [I
    new-array v2, v0, [Ljava/lang/String;
    new-array v3, v0, [[I
    const/4 v4, 0x0
    instance-of v5, v1, Ljava/lang/Object;
    or-int/2addr v4, v5
    instance-of v5, v2, [Ljava/lang/Object;
    shl-int/lit8 v5, v5, 0x1
    or-int/2addr v4, v5
    instance-of v5, v1, [Ljava/lang/Object;
    shl-int/lit8 v5, v5, 0x2
    or-int/2addr v4, v5
    instance-of v5, v3, [Ljava/lang/Object;
    shl-int/lit8 v5, v5, 0x3
    or-int/2addr v4, v5
    instance-of v5, v2, Ljava/lang/Cloneable;
    shl-int/lit8 v5, v5, 0x4
    or-int/2addr v4, v5
    instance-of v5, v1, [J
    shl-int/lit8 v5, v5, 0x5
    or-int/2addr v4, v5
    instance-of v5, v2, [Ljava/lang/CharSequence;
    shl-int/lit8 v5, v5, 0x6
    or-int/2addr v4, v5
    return v4
.end method
