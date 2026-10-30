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
