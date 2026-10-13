# Constants in every width, and arithmetic that mixes them. (One method per arithmetic instruction is in
# the generated t.Ops; this is what that does not reach.)
.class public final Lt/Arith;
.super Ljava/lang/Object;

.method public static sum(II)I
    .registers 3
    add-int v0, p0, p1
    return v0
.end method

# const/4, const/16, const, const/high16, each with a negative
.method public static consts()I
    .registers 4
    const/4 v0, 0x7
    const/4 v1, -0x8
    add-int/2addr v0, v1
    const/16 v1, 0x1234
    add-int/2addr v0, v1
    const/16 v1, -0x8000
    add-int/2addr v0, v1
    const v1, 0x12345678
    xor-int/2addr v0, v1
    const v1, -0x1
    xor-int/2addr v0, v1
    const/high16 v1, 0x7f030000
    add-int/2addr v0, v1
    const/high16 v1, -0x80000000
    xor-int/2addr v0, v1
    return v0
.end method

# const-wide/16, const-wide/32, const-wide, const-wide/high16
.method public static wideConsts()J
    .registers 6
    const-wide/16 v0, 0x7fff
    const-wide/16 v2, -0x2
    add-long/2addr v0, v2
    const-wide/32 v2, 0x12345678
    add-long/2addr v0, v2
    const-wide/32 v2, -0x80000000
    add-long/2addr v0, v2
    const-wide v2, 0x123456789abcdef0L
    xor-long/2addr v0, v2
    const-wide/high16 v2, 0x4000000000000000L
    add-long/2addr v0, v2
    const-wide/high16 v2, -0x8000000000000000L
    xor-long/2addr v0, v2
    return-wide v0
.end method

# floats written as numbers and as bits
.method public static floatConsts()F
    .registers 3
    const v0, 1.5f
    const v1, 0x40200000
    add-float/2addr v0, v1
    const/high16 v1, 0x41200000
    mul-float/2addr v0, v1
    return v0
.end method

.method public static doubleConsts()D
    .registers 4
    const-wide v0, 1.25
    const-wide/high16 v2, 0x4000000000000000L
    mul-double/2addr v0, v2
    const-wide v2, 0x3fb999999999999aL
    add-double/2addr v0, v2
    return-wide v0
.end method

# n! in a long, by a loop: a backward branch, a comparison of an int with a constant, int-to-long.
.method public static factorial(I)J
    .registers 6
    const-wide/16 v0, 0x1
    const/4 v2, 0x2
    :loop
    if-gt v2, p0, :done
    int-to-long v3, v2
    mul-long/2addr v0, v3
    add-int/lit8 v2, v2, 0x1
    goto :loop
    :done
    return-wide v0
.end method

# The sum of 1..n in a loop that counts down: what the benchmark runs.
.method public static sumTo(I)I
    .registers 3
    const/4 v0, 0x0
    :loop
    if-lez p0, :done
    add-int/2addr v0, p0
    add-int/lit8 p0, p0, -0x1
    goto :loop
    :done
    return v0
.end method

# Greatest common divisor, by recursion: a call whose result feeds the next.
.method public static gcd(II)I
    .registers 3
    if-nez p1, :more
    return p0
    :more
    rem-int v0, p0, p1
    invoke-static {p1, v0}, Lt/Arith;->gcd(II)I
    move-result v0
    return v0
.end method

.method public static fib(I)I
    .registers 3
    const/4 v0, 0x2
    if-ge p0, v0, :more
    return p0
    :more
    add-int/lit8 v0, p0, -0x1
    invoke-static {v0}, Lt/Arith;->fib(I)I
    move-result v0
    add-int/lit8 v1, p0, -0x2
    invoke-static {v1}, Lt/Arith;->fib(I)I
    move-result v1
    add-int/2addr v0, v1
    return v0
.end method
