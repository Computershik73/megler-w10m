# java.nio's buffers as code compiled against a later android.jar calls them: ByteBuffer.position(I)Ljava/nio/ByteBuffer;
# and the other covariant returns of Java 9 (and of Android 14's libcore), which Android 9's libcore has only as Buffer's.
# Run on libcore only: the built-in java.lang has no java.nio.
.class public final Lt/Nio;
.super Ljava/lang/Object;

# A buffer of 16: position 4, limit 10 (6 left), flipped (4 left, from 0), cleared (16 left), position 3 and rewound (16
# left): 6 * 1000 + 4 * 100 + 16 * 10 + 16 = 6576, every call through the buffer's own type.
.method public static chain()I
    .registers 4
    const/16 v0, 16
    invoke-static {v0}, Ljava/nio/ByteBuffer;->allocate(I)Ljava/nio/ByteBuffer;
    move-result-object v0
    const/4 v1, 4
    invoke-virtual {v0, v1}, Ljava/nio/ByteBuffer;->position(I)Ljava/nio/ByteBuffer;
    move-result-object v0
    const/16 v1, 10
    invoke-virtual {v0, v1}, Ljava/nio/ByteBuffer;->limit(I)Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->remaining()I
    move-result v1
    mul-int/lit16 v3, v1, 1000
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->flip()Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->remaining()I
    move-result v1
    mul-int/lit8 v1, v1, 100
    add-int/2addr v3, v1
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->clear()Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->remaining()I
    move-result v1
    mul-int/lit8 v1, v1, 10
    add-int/2addr v3, v1
    const/4 v1, 3
    invoke-virtual {v0, v1}, Ljava/nio/ByteBuffer;->position(I)Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->mark()Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->rewind()Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->remaining()I
    move-result v1
    add-int/2addr v3, v1
    return v3
.end method

# The same for another kind of buffer, and a name that is not one of them, which stays a NoSuchMethodError.
.method public static chars()I
    .registers 3
    const/16 v0, 8
    invoke-static {v0}, Ljava/nio/CharBuffer;->allocate(I)Ljava/nio/CharBuffer;
    move-result-object v0
    const/4 v1, 2
    invoke-virtual {v0, v1}, Ljava/nio/CharBuffer;->position(I)Ljava/nio/CharBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/CharBuffer;->remaining()I
    move-result v1
    return v1
.end method

.method public static stranger()I
    .registers 2
    const/16 v0, 8
    invoke-static {v0}, Ljava/nio/ByteBuffer;->allocate(I)Ljava/nio/ByteBuffer;
    move-result-object v0
    invoke-virtual {v0}, Ljava/nio/ByteBuffer;->capacity()Ljava/nio/ByteBuffer;
    move-result-object v0
    const/4 v1, 1
    return v1
.end method
