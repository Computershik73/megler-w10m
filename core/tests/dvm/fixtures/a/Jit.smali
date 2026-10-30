# Loops for the JIT (core/src/dvm/mg_dvm_jit.c): each method has a loop, so that with the threshold at 1 its code is
# compiled at the first jump back, and each reaches what the compiler does -- int arithmetic in every form, the shifts'
# counts, division and remainder at their edges, arrays of every element size, branches of every condition -- and the
# hand-backs to the interpreter: a divisor of zero, an index out of bounds and a null array inside a compiled loop must
# throw as interpreted, and a call in a loop's body goes out and comes back in. The test (mg_dvm_tests.c, test_jit)
# runs them compiled and checks what they return.
.class public final Lt/Jit;
.super Ljava/lang/Object;

# Sum of a[i] for every i: aget, array-length, if-ge, add-int/2addr, add-int/lit8, goto back.
.method public static sum([I)I
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

# The same over bytes, chars, shorts and booleans: the element sizes and their signs.
.method public static sumBytes([B)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-byte v3, p0, v1
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static sumChars([C)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-char v3, p0, v1
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static sumShorts([S)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-short v3, p0, v1
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

.method public static countTrue([Z)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-boolean v3, p0, v1
    if-eqz v3, :next
    add-int/lit8 v0, v0, 0x1
    :next
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# Fills a[i] = i * k for each element size and reads them back: aput of each size, mul-int.
.method public static fill([I[B[C[SI)I
    .registers 10
    const/4 v0, 0x0
    array-length v1, p0
    :loop
    if-ge v0, v1, :done
    mul-int v2, v0, p4
    aput v2, p0, v0
    int-to-byte v3, v2
    aput-byte v3, p1, v0
    int-to-char v3, v2
    aput-char v3, p2, v0
    int-to-short v3, v2
    aput-short v3, p3, v0
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    invoke-static {p0}, Lt/Jit;->sum([I)I
    move-result v0
    invoke-static {p1}, Lt/Jit;->sumBytes([B)I
    move-result v1
    add-int/2addr v0, v1
    invoke-static {p2}, Lt/Jit;->sumChars([C)I
    move-result v1
    add-int/2addr v0, v1
    invoke-static {p3}, Lt/Jit;->sumShorts([S)I
    move-result v1
    add-int/2addr v0, v1
    return v0
.end method

# A mix of every int operation, n times, on a running value: what an interpreter and its compiled code must agree on.
.method public static mix(II)I
    .registers 9
    move v0, p1
    const/4 v1, 0x0
    :loop
    if-ge v1, p0, :done
    add-int v2, v0, v1
    sub-int v3, v2, p1
    mul-int v2, v3, v2
    xor-int v2, v2, v0
    or-int/lit16 v3, v1, 0x1
    div-int v4, v2, v3
    rem-int v5, v2, v3
    add-int/2addr v4, v5
    shl-int v5, v4, v1
    shr-int v3, v2, v1
    ushr-int v6, v2, v1
    xor-int/2addr v5, v3
    add-int/2addr v5, v6
    and-int/lit8 v3, v5, 0x7f
    not-int v6, v5
    neg-int v6, v6
    sub-int/2addr v6, v3
    rsub-int v3, v6, 0x1234
    rsub-int/lit8 v3, v3, 0x11
    mul-int/lit16 v3, v3, -0x3
    div-int/lit8 v4, v3, 0x7
    rem-int/lit16 v5, v3, 0x65
    add-int/2addr v4, v5
    shl-int/lit8 v5, v4, 0x21
    shr-int/lit8 v6, v4, 0x3
    ushr-int/lit8 v3, v4, 0x1f
    add-int/2addr v5, v6
    add-int/2addr v5, v3
    and-int v0, v5, v2
    or-int/2addr v0, v5
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# Integer.MIN_VALUE / -1 and % -1, in a loop: Java's answers (MIN_VALUE and 0), not a trap.
.method public static minOverMinusOne(I)I
    .registers 6
    const/high16 v0, -0x80000000
    const/4 v1, -0x1
    const/4 v2, 0x0
    const/4 v3, 0x0
    :loop
    if-ge v3, p0, :done
    div-int v4, v0, v1
    add-int/2addr v2, v4
    rem-int v4, v0, v1
    add-int/2addr v2, v4
    add-int/lit8 v3, v3, 0x1
    goto :loop
    :done
    return v2
.end method

# Each condition of if-test and if-testz counted over -n..n: lt, le, gt, ge, eq, ne against 0 and against a register.
.method public static conditions(I)I
    .registers 6
    neg-int v0, p0
    const/4 v1, 0x0
    const/4 v2, 0x0
    :loop
    if-gt v0, p0, :done
    if-gez v0, :a
    add-int/lit8 v1, v1, 0x1
    :a
    if-lez v0, :b
    add-int/lit8 v1, v1, 0x2
    :b
    if-nez v0, :c
    add-int/lit8 v1, v1, 0x4
    :c
    if-ltz v0, :d
    add-int/lit8 v1, v1, 0x8
    :d
    if-gtz v0, :e
    add-int/lit8 v1, v1, 0x10
    :e
    if-eqz v0, :f
    add-int/lit8 v1, v1, 0x20
    :f
    if-lt v0, v2, :g
    add-int/lit16 v1, v1, 0x40
    :g
    if-le v0, v2, :h
    add-int/lit16 v1, v1, 0x80
    :h
    if-eq v0, v2, :i
    add-int/lit16 v1, v1, 0x100
    :i
    if-ne v0, v2, :j
    add-int/lit16 v1, v1, 0x200
    :j
    if-ge v0, v2, :k
    add-int/lit16 v1, v1, 0x400
    :k
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    return v1
.end method

# Divides by a[i] for each i: a zero among them throws ArithmeticException from inside the compiled loop.
.method public static divideAll(I[I)I
    .registers 6
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p1
    :loop
    if-ge v1, v2, :done
    aget v3, p1, v1
    div-int v3, p0, v3
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# Reads a[0..n): past the end it throws ArrayIndexOutOfBoundsException from inside the compiled loop; a null array, NPE.
.method public static readTo([II)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    :loop
    if-ge v1, p1, :done
    aget v2, p0, v1
    add-int/2addr v0, v2
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# A call in the loop's body: out to the interpreter for it and back in at the next jump back.
.method public static callsInLoop(I)I
    .registers 4
    const/4 v0, 0x0
    const/4 v1, 0x0
    :loop
    if-ge v1, p0, :done
    invoke-static {v1, v0}, Lt/Arith;->sum(II)I
    move-result v0
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method

# Long values moved in a loop (move-wide) beside int ones: the pairs kept whole.
.method public static wideMoves(IJ)J
    .registers 10
    move-wide v0, p1
    const/4 v2, 0x0
    :loop
    if-ge v2, p0, :done
    move-wide v4, v0
    move-wide/from16 v0, v4
    add-int/lit8 v2, v2, 0x1
    goto :loop
    :done
    return-wide v0
.end method

# Fields in a loop: iget and iput of every width, an object field, sget and sput (the class initialised before the loop).
# Returns i + b + c + s + z + total, or -1 when the object field or the long ones did not keep what was put.
.method public static fields(I)I
    .registers 8
    new-instance v0, Lt/JitPoint;
    invoke-direct {v0}, Lt/JitPoint;-><init>()V
    const-wide v4, 0x1122334455667788L
    iput-wide v4, v0, Lt/JitPoint;->j:J
    const/4 v1, 0x0
    sput v1, Lt/JitPoint;->total:I
    sput-wide v4, Lt/JitPoint;->wide:J
    :loop
    if-ge v1, p0, :done
    iget v2, v0, Lt/JitPoint;->i:I
    add-int/2addr v2, v1
    iput v2, v0, Lt/JitPoint;->i:I
    iget-byte v3, v0, Lt/JitPoint;->b:B
    add-int/lit8 v3, v3, 0x7
    iput-byte v3, v0, Lt/JitPoint;->b:B
    iget-char v3, v0, Lt/JitPoint;->c:C
    add-int/lit16 v3, v3, 0x3e8
    iput-char v3, v0, Lt/JitPoint;->c:C
    iget-short v3, v0, Lt/JitPoint;->s:S
    add-int/lit16 v3, v3, 0x3e8
    iput-short v3, v0, Lt/JitPoint;->s:S
    iget-boolean v3, v0, Lt/JitPoint;->z:Z
    xor-int/lit8 v3, v3, 0x1
    iput-boolean v3, v0, Lt/JitPoint;->z:Z
    iget-wide v4, v0, Lt/JitPoint;->j:J
    iput-wide v4, v0, Lt/JitPoint;->j:J
    sget-wide v4, Lt/JitPoint;->wide:J
    sput-wide v4, Lt/JitPoint;->wide:J
    iput-object v0, v0, Lt/JitPoint;->o:Ljava/lang/Object;
    iget-object v3, v0, Lt/JitPoint;->o:Ljava/lang/Object;
    sget v3, Lt/JitPoint;->total:I
    add-int/2addr v3, v1
    sput v3, Lt/JitPoint;->total:I
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    iget-object v3, v0, Lt/JitPoint;->o:Ljava/lang/Object;
    if-ne v3, v0, :bad
    iget-wide v4, v0, Lt/JitPoint;->j:J
    const-wide v2, 0x1122334455667788L
    cmp-long v6, v4, v2
    if-nez v6, :bad
    sget-wide v4, Lt/JitPoint;->wide:J
    cmp-long v6, v4, v2
    if-nez v6, :bad
    iget v2, v0, Lt/JitPoint;->i:I
    iget-byte v3, v0, Lt/JitPoint;->b:B
    add-int/2addr v2, v3
    iget-char v3, v0, Lt/JitPoint;->c:C
    add-int/2addr v2, v3
    iget-short v3, v0, Lt/JitPoint;->s:S
    add-int/2addr v2, v3
    iget-boolean v3, v0, Lt/JitPoint;->z:Z
    add-int/2addr v2, v3
    sget v3, Lt/JitPoint;->total:I
    add-int/2addr v2, v3
    return v2
    :bad
    const/4 v2, -0x1
    return v2
.end method

# Calls of each kind in a compiled loop, their results through move-result: an interface method and a virtual one (trivial
# getters, done in place), a static one by both forms of invoke, a long returned, and String.length (an intrinsic).
.method public static callKinds(I)I
    .registers 10
    new-instance v0, Lt/Blob;
    const/4 v1, 0x3
    invoke-direct {v0, v1}, Lt/Blob;-><init>(I)V
    const-string v4, "abcd"
    const/4 v1, 0x0
    const/4 v2, 0x0
    :loop
    if-ge v1, p0, :done
    invoke-interface {v0}, Lt/HasArea;->area()I
    move-result v3
    add-int/2addr v2, v3
    invoke-virtual {v0}, Lt/Blob;->hashCode()I
    move-result v3
    add-int/2addr v2, v3
    invoke-static {v1, v2}, Lt/Arith;->sum(II)I
    move-result v3
    add-int/2addr v2, v3
    invoke-static/range {v1 .. v2}, Lt/Arith;->sum(II)I
    move-result v3
    xor-int/2addr v2, v3
    invoke-virtual {v4}, Ljava/lang/String;->length()I
    move-result v3
    add-int/2addr v2, v3
    invoke-static {v1}, Lt/Jit;->wide(I)J
    move-result-wide v6
    long-to-int v3, v6
    add-int/2addr v2, v3
    const-string v5, "abcd"
    if-ne v5, v4, :bad
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v2
    :bad
    const/4 v2, -0x1
    return v2
.end method

.method public static wide(I)J
    .registers 3
    int-to-long v0, p0
    const-wide/16 v0, 0x0
    int-to-long v0, p0
    return-wide v0
.end method

# A callee that throws inside a compiled loop, caught by the loop's own handler: 100 / (i - 2) summed, 1000 for the zero.
.method public static catchInLoop(I)I
    .registers 6
    const/4 v0, 0x0
    const/4 v1, 0x0
    const/16 v4, 0x64
    :loop
    if-ge v1, p0, :done
    add-int/lit8 v2, v1, -0x2
    :try_start
    invoke-static {v4, v2}, Lt/Exc;->wrongCatch(II)I
    move-result v3
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :handler
    add-int/2addr v0, v3
    :next
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :handler
    move-exception v3
    add-int/lit16 v0, v0, 0x3e8
    goto :next
    :done
    return v0
.end method

# Recursion through compiled calls, n deep: each level goes round a short loop first, so that its code is entered, and
# makes the call from the code. Past the depth the host's stack allows another loop for, the calls go on interpreted; the
# answer is n either way.
.method public static deepCalls(I)I
    .registers 4
    if-gtz p0, :more
    const/4 v0, 0x0
    return v0
    :more
    const/4 v1, 0x0
    const/4 v2, 0x2
    :warm
    if-ge v1, v2, :go
    add-int/lit8 v1, v1, 0x1
    goto :warm
    :go
    add-int/lit8 v2, p0, -0x1
    invoke-static {v2}, Lt/Jit;->deepCalls(I)I
    move-result v0
    add-int/lit8 v0, v0, 0x1
    return v0
.end method

# Long arithmetic in a compiled loop: a 64-bit LCG (mul-long, add-long), shifts of each kind by counts below and above 32,
# and, or, xor, neg, not, int-to-long, long-to-int and cmp-long, all folded into one long.
.method public static longs(IJ)J
    .registers 16
    move-wide v0, p1
    const-wide/16 v2, 0x0
    const/4 v4, 0x0
    :loop
    if-ge v4, p0, :done
    const-wide v6, 0x5851f42d4c957f2dL
    mul-long/2addr v0, v6
    const-wide v6, 0x14057b7ef767814fL
    add-long/2addr v0, v6
    and-int/lit8 v5, v4, 0x3f
    ushr-long v6, v0, v5
    xor-long/2addr v2, v6
    shl-long v6, v0, v5
    add-long/2addr v2, v6
    shr-long v6, v0, v5
    sub-long/2addr v2, v6
    or-long v8, v0, v2
    and-long v6, v8, v0
    xor-long/2addr v2, v6
    neg-long v6, v2
    not-long v8, v6
    add-long/2addr v2, v8
    int-to-long v6, v4
    add-long/2addr v2, v6
    long-to-int v5, v2
    int-to-long v6, v5
    xor-long/2addr v2, v6
    cmp-long v5, v0, v2
    int-to-long v6, v5
    add-long/2addr v2, v6
    const-wide/high16 v6, 0x4000000000000000L
    cmp-long v5, v2, v6
    int-to-long v6, v5
    add-long/2addr v2, v6
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :done
    add-long/2addr v0, v2
    return-wide v0
.end method

# Switches in a compiled loop: a packed one over i % 6 - 1 (keys -1 to 3, 4 missing it) and a sparse one over i * 7 % 50.
.method public static switches(I)I
    .registers 6
    const/4 v0, 0x0
    const/4 v1, 0x0
    :loop
    if-ge v1, p0, :done
    rem-int/lit8 v2, v1, 0x6
    add-int/lit8 v2, v2, -0x1
    packed-switch v2, :ptable
    add-int/lit8 v0, v0, 0x1
    goto :sparse
    :p0
    add-int/lit8 v0, v0, 0xa
    goto :sparse
    :p1
    add-int/lit8 v0, v0, 0x14
    :p2
    add-int/lit8 v0, v0, 0x1e
    goto :sparse
    :p3
    add-int/lit8 v0, v0, 0x28
    :sparse
    mul-int/lit8 v2, v1, 0x7
    rem-int/lit8 v2, v2, 0x32
    sparse-switch v2, :stable
    add-int/lit8 v0, v0, 0x2
    goto :next
    :s0
    add-int/lit16 v0, v0, 0x64
    goto :next
    :s1
    add-int/lit16 v0, v0, 0xc8
    goto :next
    :s2
    add-int/lit16 v0, v0, 0x12c
    :next
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
    :ptable
    .packed-switch -0x1
        :p0
        :p1
        :p2
        :p3
    .end packed-switch
    :stable
    .sparse-switch
        0x0 -> :s0
        0x7 -> :s1
        0x31 -> :s2
    .end sparse-switch
.end method
