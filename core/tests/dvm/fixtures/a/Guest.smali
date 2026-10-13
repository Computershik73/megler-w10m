# Native methods in a guest library (core/tests/guest/t_jni.c), and what that library calls back.
# The methods with no code are the library's; the rest is what it reaches through JNI.
.class public Lt/Guest;
.super Ljava/lang/Object;

.field public count:I
.field public static total:I

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lt/Guest;->count:I
    return-void
.end method

# Found by its JNI name, Java_t_Guest_add; the others are registered by JNI_OnLoad.
.method public static native add(II)I
.end method

.method public static native mix(IJDFLjava/lang/String;[I)J
.end method

.method public static native many(JJJJJJJJJJJJJJ)J
.end method

.method public static native greet(Ljava/lang/String;)Ljava/lang/String;
.end method

.method public static native squares(I)[I
.end method

.method public static native words()[Ljava/lang/String;
.end method

.method public native callBack(I)I
.end method

.method public static native thrower()V
.end method

.method public static native catcher()I
.end method

.method public static native make(I)Lt/Guest;
.end method

.method public static native fmul(DF)D
.end method

.method public native locked()I
.end method

.method public static twice(I)I
    .registers 2
    mul-int/lit8 v0, p0, 0x2
    return v0
.end method

.method public scale(I)I
    .registers 3
    iget v0, p0, Lt/Guest;->count:I
    mul-int/2addr v0, p1
    return v0
.end method

.method public static boom()I
    .registers 2
    const/4 v0, 0x1
    const/4 v1, 0x0
    div-int/2addr v0, v1
    return v0
.end method

.method public static sumArray([I)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget v3, p0, v1
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# "a" + "," + "b"... of a String[]
.method public static join([Ljava/lang/String;)Ljava/lang/String;
    .registers 5
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    if-eqz v1, :first
    const/16 v3, 0x2c
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;
    :first
    aget-object v3, p0, v1
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# mix(7, 1 << 40, 2.5, 1.5f, "abc", {1, 2, 3}): every kind of argument across the boundary, from bytecode.
.method public static runMix()J
    .registers 10
    const/4 v0, 0x7
    const-wide v1, 0x10000000000L
    const-wide/high16 v3, 0x4004000000000000L
    const/high16 v5, 0x3fc00000
    const-string v6, "abc"
    const/4 v7, 0x3
    new-array v7, v7, [I
    fill-array-data v7, :data
    invoke-static/range {v0 .. v7}, Lt/Guest;->mix(IJDFLjava/lang/String;[I)J
    move-result-wide v0
    return-wide v0
    :data
    .array-data 4
        0x1 0x2 0x3
    .end array-data
.end method

# An exception thrown by the library, caught here: the length of its message.
.method public static runCatch()I
    .registers 2
    :try_start
    invoke-static {}, Lt/Guest;->thrower()V
    :try_end
    .catch Ljava/lang/IllegalStateException; {:try_start .. :try_end} :handler
    const/4 v0, -0x1
    return v0
    :handler
    move-exception v0
    invoke-virtual {v0}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/String;->length()I
    move-result v0
    return v0
.end method

# What the library's array and strings hold, read by bytecode: squares(5) summed, and words() joined.
.method public static runArrays()Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const/4 v1, 0x5
    invoke-static {v1}, Lt/Guest;->squares(I)[I
    move-result-object v1
    invoke-static {v1}, Lt/Guest;->sumArray([I)I
    move-result v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    const/16 v1, 0x3a
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;
    invoke-static {}, Lt/Guest;->words()[Ljava/lang/String;
    move-result-object v1
    invoke-static {v1}, Lt/Guest;->join([Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
