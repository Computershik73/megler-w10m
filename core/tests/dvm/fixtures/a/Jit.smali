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

# Objects in a compiled loop: new-instance with its constructor called, instance-of, check-cast, a call on it, new-array
# and its length, aput-object and aget-object, a monitor taken and let go, const-class; instance-of and check-cast of the
# very class and of null (the code's own answer) and of an interface (the runtime's). Each turn adds 3 + i + (i & 7).
.method public static objects(I)I
    .registers 10
    const/4 v0, 0x0
    const/4 v1, 0x0
    const/4 v2, 0x4
    new-array v3, v2, [Ljava/lang/Object;
    :loop
    if-ge v1, p0, :done
    new-instance v4, Lt/Blob;
    invoke-direct {v4, v1}, Lt/Blob;-><init>(I)V
    instance-of v5, v4, Lt/HasArea;
    add-int/2addr v0, v5
    instance-of v5, v4, Lt/Blob;
    add-int/2addr v0, v5
    const/4 v6, 0x0
    instance-of v5, v6, Lt/Blob;
    add-int/2addr v0, v5
    check-cast v6, Lt/Blob;
    check-cast v4, Lt/Blob;
    invoke-virtual {v4}, Lt/Blob;->area()I
    move-result v5
    add-int/2addr v0, v5
    and-int/lit8 v5, v1, 0x7
    new-array v6, v5, [I
    array-length v5, v6
    add-int/2addr v0, v5
    and-int/lit8 v5, v1, 0x3
    aput-object v4, v3, v5
    aget-object v6, v3, v5
    if-ne v6, v4, :bad
    monitor-enter v4
    add-int/lit8 v0, v0, 0x1
    monitor-exit v4
    const-class v6, Lt/Blob;
    if-eqz v6, :bad
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
    :bad
    const/4 v0, -0x1
    return v0
.end method

# A String stored into a Blob[] at turn 3 of a compiled loop: ArrayStoreException, caught after the loop; 1000 + the turn.
.method public static storeFails(I)I
    .registers 8
    const/4 v0, 0x4
    new-array v1, v0, [Lt/Blob;
    const-string v2, "x"
    new-instance v3, Lt/Blob;
    const/4 v4, 0x1
    invoke-direct {v3, v4}, Lt/Blob;-><init>(I)V
    const/4 v4, 0x0
    :try_start
    :loop
    if-ge v4, p0, :done
    move-object v5, v3
    const/4 v6, 0x3
    if-ne v4, v6, :store
    move-object v5, v2
    :store
    and-int/lit8 v6, v4, 0x3
    aput-object v5, v1, v6
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :try_end
    .catch Ljava/lang/ArrayStoreException; {:try_start .. :try_end} :handler
    :done
    const/4 v0, -0x1
    return v0
    :handler
    move-exception v5
    add-int/lit16 v0, v4, 0x3e8
    return v0
.end method

# A String cast to Blob at turn 5 of a compiled loop: ClassCastException, caught after the loop; 2000 + the turn.
.method public static castFails(I)I
    .registers 8
    const-string v2, "y"
    new-instance v3, Lt/Blob;
    const/4 v4, 0x1
    invoke-direct {v3, v4}, Lt/Blob;-><init>(I)V
    const/4 v4, 0x0
    :try_start
    :loop
    if-ge v4, p0, :done
    move-object v5, v3
    const/4 v6, 0x5
    if-ne v4, v6, :cast
    move-object v5, v2
    :cast
    check-cast v5, Lt/Blob;
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :try_end
    .catch Ljava/lang/ClassCastException; {:try_start .. :try_end} :handler
    :done
    const/4 v0, -0x1
    return v0
    :handler
    move-exception v5
    add-int/lit16 v0, v4, 0x7d0
    return v0
.end method

# Calls of trivial methods in a loop, done in the code (mg_dvm_jit.c, inline_call). Each turn sets and gets i through an
# object that is a JitShape three turns in four and a JitShapeMore the fourth (whose getI overrides, adding 100), and adds
# seven(), total() (5), getC() after setC(i), the low word of getJ() (0x1234) and calls touch(); getO() of the JitShapeMore
# is itself. At turn 50 the object is null: the NullPointerException of setI's call is caught in the loop, 1000.
.method public static inlined(I)I
    .registers 9
    new-instance v0, Lt/JitShape;
    invoke-direct {v0}, Lt/JitShape;-><init>()V
    new-instance v1, Lt/JitShapeMore;
    invoke-direct {v1}, Lt/JitShapeMore;-><init>()V
    const/4 v2, 0x5
    sput v2, Lt/JitShape;->total:I
    const/4 v2, 0x0
    const/4 v3, 0x0
    :loop
    if-ge v2, p0, :done
    move-object v4, v0
    and-int/lit8 v5, v2, 0x3
    const/4 v6, 0x3
    if-ne v5, v6, :plain
    move-object v4, v1
    :plain
    const/16 v6, 0x32
    if-ne v2, v6, :call
    const/4 v4, 0x0
    :call
    :try_start
    invoke-virtual {v4, v2}, Lt/JitShape;->setI(I)V
    invoke-virtual {v4}, Lt/JitShape;->getI()I
    move-result v5
    :try_end
    .catch Ljava/lang/NullPointerException; {:try_start .. :try_end} :caught
    add-int/2addr v3, v5
    invoke-virtual {v0}, Lt/JitShape;->seven()I
    move-result v5
    add-int/2addr v3, v5
    invoke-static {}, Lt/JitShape;->total()I
    move-result v5
    add-int/2addr v3, v5
    invoke-virtual {v0, v2}, Lt/JitShape;->setC(C)V
    invoke-virtual {v0}, Lt/JitShape;->getC()C
    move-result v5
    add-int/2addr v3, v5
    invoke-virtual {v0}, Lt/JitShape;->touch()V
    invoke-virtual {v0}, Lt/JitShape;->getJ()J
    move-result-wide v6
    long-to-int v5, v6
    add-int/2addr v3, v5
    invoke-virtual {v1}, Lt/JitShape;->getO()Ljava/lang/Object;
    move-result-object v5
    if-ne v5, v1, :bad
    :next
    add-int/lit8 v2, v2, 0x1
    goto :loop
    :caught
    move-exception v5
    add-int/lit16 v3, v3, 0x3e8
    goto :next
    :done
    return v3
    :bad
    const/4 v3, -0x1
    return v3
.end method

# A loop that returns from inside, compiled whole now that returns are: the index of the first element above the limit, or -1.
.method public static firstAbove([II)I
    .registers 5
    const/4 v0, 0x0
    array-length v1, p0
    :loop
    if-ge v0, v1, :none
    aget v2, p0, v0
    if-le v2, p1, :next
    return v0
    :next
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :none
    const/4 v0, -0x1
    return v0
.end method

# A method without loops for the test of code that waits for its page (mg_dvm_jit.c, place): ((n + 1) * 3) ^ 5, called
# from callWaits so that its calls are counted (a call from C is not).
.method public static waits(I)I
    .registers 2
    add-int/lit8 v0, p0, 0x1
    mul-int/lit8 v0, v0, 0x3
    xor-int/lit8 v0, v0, 0x5
    return v0
.end method

.method public static callWaits(I)I
    .registers 2
    invoke-static {p0}, Lt/Jit;->waits(I)I
    move-result v0
    return v0
.end method

# Floating point in a compiled loop: float and double arithmetic in both forms, cmpl and cmpg (NaN among them), the
# conversions between int, float and double, neg, and a double[] read and written (jit_floats in mg_dvm_tests.c says the
# same in C).
.method public static floats(I)D
    .registers 13
    const/high16 v0, 0x3f000000
    const-wide/high16 v1, 0x3fd0000000000000L
    const/4 v3, 0x0
    const/4 v4, 0x0
    const/4 v5, 0x4
    new-array v11, v5, [D
    :loop
    if-ge v4, p0, :done
    int-to-float v5, v4
    const/high16 v10, 0x3f400000
    mul-float/2addr v0, v10
    const/high16 v10, 0x40400000
    div-float v10, v5, v10
    add-float/2addr v0, v10
    const/high16 v10, 0x3e800000
    sub-float/2addr v0, v10
    int-to-double v6, v4
    const-wide/high16 v8, 0x3fe0000000000000L
    mul-double/2addr v1, v8
    const-wide/high16 v8, 0x401c000000000000L
    div-double v8, v6, v8
    add-double/2addr v1, v8
    float-to-double v8, v0
    sub-double/2addr v1, v8
    float-to-int v10, v0
    add-int/2addr v3, v10
    const-wide/high16 v8, 0x4008000000000000L
    mul-double v8, v1, v8
    double-to-int v10, v8
    add-int/2addr v3, v10
    cmpl-float v10, v0, v5
    add-int/2addr v3, v10
    cmpg-double v10, v1, v6
    add-int/2addr v3, v10
    neg-float v0, v0
    neg-float v0, v0
    neg-double v1, v1
    neg-double v1, v1
    double-to-float v5, v1
    const/high16 v10, 0x7fc00000
    cmpl-float v10, v5, v10
    add-int/2addr v3, v10
    const/high16 v10, 0x7fc00000
    cmpg-float v10, v10, v5
    add-int/2addr v3, v10
    cmpl-double v10, v1, v1
    add-int/2addr v3, v10
    mul-float v10, v5, v5
    sub-float/2addr v10, v5
    float-to-int v10, v10
    add-int/2addr v3, v10
    and-int/lit8 v10, v4, 0x3
    aput-wide v1, v11, v10
    add-int/lit8 v10, v4, 0x1
    and-int/lit8 v10, v10, 0x3
    aget-wide v6, v11, v10
    const-wide/high16 v8, 0x3fd0000000000000L
    mul-double/2addr v6, v8
    add-double/2addr v1, v6
    sub-double v6, v1, v6
    add-double/2addr v1, v6
    mul-double/2addr v1, v8
    const-wide/high16 v8, 0x4010000000000000L
    div-double/2addr v1, v8
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :done
    float-to-double v6, v0
    add-double/2addr v1, v6
    int-to-double v6, v3
    add-double/2addr v1, v6
    return-wide v1
.end method

# By calls from a compiled loop (mg_dvm_jit.c, "by calls"): a long's division and remainder in both forms, a long's
# conversions to and from float and double, a float's and a double's remainder in both forms. The divisor is i - 7: at turn
# 7 it is zero, and the ArithmeticException is caught in the loop, 1000 (jit_by_calls in mg_dvm_tests.c says it in C).
.method public static byCalls(IJ)J
    .registers 14
    move-wide v0, p1
    const-wide/16 v2, 0x0
    const/4 v4, 0x0
    :loop
    if-ge v4, p0, :done
    add-int/lit8 v5, v4, -0x7
    int-to-long v6, v5
    :try_start
    div-long v8, v0, v6
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :caught
    add-long/2addr v2, v8
    rem-long v8, v0, v6
    xor-long/2addr v2, v8
    move-wide v8, v0
    div-long/2addr v8, v6
    sub-long/2addr v2, v8
    move-wide v8, v0
    rem-long/2addr v8, v6
    add-long/2addr v2, v8
    long-to-float v5, v0
    float-to-long v8, v5
    add-long/2addr v2, v8
    long-to-double v8, v0
    double-to-long v8, v8
    xor-long/2addr v2, v8
    int-to-float v5, v4
    const/high16 v10, 0x40400000
    rem-float v5, v5, v10
    float-to-long v8, v5
    add-long/2addr v2, v8
    int-to-float v5, v4
    const/high16 v10, 0x40e00000
    rem-float/2addr v5, v10
    float-to-long v8, v5
    xor-long/2addr v2, v8
    int-to-double v6, v4
    const-wide/high16 v8, 0x4020000000000000L
    rem-double v6, v6, v8
    double-to-long v8, v6
    add-long/2addr v2, v8
    int-to-double v6, v4
    const-wide/high16 v8, 0x4014000000000000L
    rem-double/2addr v6, v8
    double-to-long v8, v6
    sub-long/2addr v2, v8
    const-wide v8, 0x5851f42d4c957f2dL
    mul-long/2addr v0, v8
    const-wide v8, 0x14057b7ef767814fL
    add-long/2addr v0, v8
    :next
    add-int/lit8 v4, v4, 0x1
    goto :loop
    :caught
    move-exception v5
    const-wide/16 v8, 0x3e8
    add-long/2addr v2, v8
    goto :next
    :done
    return-wide v2
.end method

# String.length, isEmpty and charAt, Math.abs, min and max of ints and Math.sqrt called in a compiled loop, done in the code
# (mg_dvm_jit.c, intrinsic_in_code). The last turn's charAt is past the end: its StringIndexOutOfBoundsException, from the
# call the code falls back to, is caught in the loop, 1000 (jit_intrinsics in mg_dvm_tests.c says it in C).
.method public static intrinsics(Ljava/lang/String;)I
    .registers 10
    const/4 v0, 0x0
    const/4 v1, 0x0
    invoke-virtual {p0}, Ljava/lang/String;->length()I
    move-result v2
    :loop
    if-gt v1, v2, :done
    :try_start
    invoke-virtual {p0, v1}, Ljava/lang/String;->charAt(I)C
    move-result v3
    :try_end
    .catch Ljava/lang/StringIndexOutOfBoundsException; {:try_start .. :try_end} :caught
    add-int/2addr v0, v3
    invoke-virtual {p0}, Ljava/lang/String;->length()I
    move-result v3
    add-int/2addr v0, v3
    invoke-virtual {p0}, Ljava/lang/String;->isEmpty()Z
    move-result v3
    add-int/2addr v0, v3
    add-int/lit8 v4, v1, -0x5
    invoke-static {v4}, Ljava/lang/Math;->abs(I)I
    move-result v3
    add-int/2addr v0, v3
    invoke-static {v1, v4}, Ljava/lang/Math;->min(II)I
    move-result v3
    add-int/2addr v0, v3
    invoke-static {v4, v1}, Ljava/lang/Math;->max(II)I
    move-result v3
    xor-int/2addr v0, v3
    int-to-double v5, v1
    invoke-static {v5, v6}, Ljava/lang/Math;->sqrt(D)D
    move-result-wide v5
    const-wide/high16 v7, 0x4024000000000000L
    mul-double/2addr v5, v7
    double-to-int v3, v5
    add-int/2addr v0, v3
    :next
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :caught
    move-exception v3
    add-int/lit16 v0, v0, 0x3e8
    goto :next
    :done
    return v0
.end method

# Ints made by compiled code are kept as the interpreter keeps them, zero-extended where a register is 64 bits wide, so
# that if-eq, if-ne and if-nez, which compare whole registers, find them equal to the same ints made otherwise: -1 and
# MIN_VALUE out of arithmetic, an int[], a field, int-to-byte, float-to-int, cmp-long, cmpl-float and a compiled call,
# against constants -- in the compiled loop, and after it in the interpreter, to which fill-array-data hands the frame
# back. 0 when all agree, else the number of the first that did not.
.method public static upperHalves(I)I
    .registers 16
    const/4 v0, 0x1
    new-array v1, v0, [I
    const/4 v2, -0x5
    const/4 v3, 0x0
    aput v2, v1, v3
    new-instance v4, Lt/JitPoint;
    invoke-direct {v4}, Lt/JitPoint;-><init>()V
    const/high16 v2, -0x80000000
    iput v2, v4, Lt/JitPoint;->i:I
    const/4 v5, 0x0
    :loop
    if-ge v5, p0, :done
    const/16 v15, 0x1
    sub-int v6, v5, v5
    add-int/lit8 v6, v6, -0x1
    const/4 v7, -0x1
    if-ne v6, v7, :bad
    const/16 v15, 0x2
    aget v8, v1, v3
    const/4 v9, -0x5
    if-ne v8, v9, :bad
    const/16 v15, 0x3
    iget v8, v4, Lt/JitPoint;->i:I
    const/high16 v9, -0x80000000
    if-ne v8, v9, :bad
    const/16 v15, 0x4
    const/16 v8, 0xff
    int-to-byte v8, v8
    if-ne v8, v7, :bad
    const/16 v15, 0x5
    const/high16 v8, -0x40400000
    float-to-int v8, v8
    if-ne v8, v7, :bad
    const/16 v15, 0x6
    const-wide/16 v10, 0x0
    const-wide/16 v12, 0x1
    cmp-long v8, v10, v12
    if-ne v8, v7, :bad
    const/16 v15, 0x7
    const/high16 v9, 0x7fc00000
    cmpl-float v8, v9, v9
    if-ne v8, v7, :bad
    const/16 v15, 0x8
    invoke-static {v5}, Lt/Jit;->minusOne(I)I
    move-result v8
    if-ne v8, v7, :bad
    const/16 v15, 0x9
    add-int/2addr v8, v0
    if-nez v8, :bad
    add-int/lit8 v5, v5, 0x1
    goto :loop
    :done
    fill-array-data v1, :one
    const/16 v15, 0xa
    const/4 v14, -0x1
    if-ne v6, v14, :bad
    const/16 v15, 0xb
    if-ne v7, v14, :bad
    const/16 v15, 0xc
    invoke-static {v5}, Lt/Jit;->minusOne(I)I
    move-result v8
    if-ne v8, v14, :bad
    const/16 v15, 0x0
    :bad
    return v15
    :one
    .array-data 4
        0x1
    .end array-data
.end method

# -1, not trivially (so that a call of it runs its code, which with the threshold at 1 is compiled at its first call).
.method public static minusOne(I)I
    .registers 2
    sub-int v0, p0, p0
    add-int/lit8 v0, v0, -0x1
    return v0
.end method

# A compiled method's int returned to an interpreted caller (no loop here, and C calls it): zero-extended too. 0 when so.
.method public static upperCaller()I
    .registers 4
    const/4 v0, 0x3
    invoke-static {v0}, Lt/Jit;->minusOne(I)I
    move-result v1
    const/4 v2, -0x1
    if-ne v1, v2, :bad
    const/4 v0, 0x0
    return v0
    :bad
    const/4 v0, 0x1
    return v0
.end method

# A frame of 300 registers: longs and ints computed in registers past 63, where A64's register pairs (LDP, STP) no longer
# reach, and moved to and from registers past 255 (move/16, move-wide/16), in a compiled loop. jit_big_frame in
# mg_dvm_tests.c says the same in C.
.method public static bigFrame(IJ)J
    .registers 300
    move/16 v1, v297
    move-wide/16 v100, v298
    const/4 v0, 0x0
    const-wide v102, 0x5851f42d4c957f2dL
    const-wide v104, 0x14057b7ef767814fL
    :loop
    if-ge v0, v1, :done
    mul-long v100, v100, v102
    add-long v100, v100, v104
    add-int/lit8 v150, v0, 0x7
    mul-int v151, v150, v150
    shl-long v106, v100, v151
    xor-long v100, v100, v106
    ushr-long v106, v100, v150
    xor-long v100, v100, v106
    move-wide/16 v290, v100
    move-wide/16 v108, v290
    sub-long v100, v108, v104
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    move-wide/16 v2, v100
    return-wide v2
.end method

# Reads a[2 - i] for i from 0 on: at the fourth turn, compiled by then, the index is -1, past the bounds as the code's
# unsigned compare sees it, and ArrayIndexOutOfBoundsException is thrown from inside the compiled loop.
.method public static readDown([I)I
    .registers 5
    const/4 v0, 0x0
    const/4 v1, 0x0
    :loop
    const/4 v2, 0x2
    sub-int v2, v2, v1
    aget v3, p0, v2
    add-int/2addr v0, v3
    add-int/lit8 v1, v1, 0x1
    goto :loop
.end method

# Sums o.i over an array of JitPoints: a null among them, met by the compiled loop, throws NullPointerException there.
.method public static fieldOfNull([Lt/JitPoint;)I
    .registers 6
    const/4 v0, 0x0
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    aget-object v3, p0, v1
    iget v4, v3, Lt/JitPoint;->i:I
    add-int/2addr v0, v4
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    return v0
.end method
