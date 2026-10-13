# Values of two registers: longs and doubles as arguments at every offset, moved in every form, in fields,
# passed to calls and returned from them.
.class public final Lt/Wide;
.super Ljava/lang/Object;

.field public static sj:J
.field public static sd:D
.field public j:J
.field public i:I
.field public d:D

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

# (a + b) * c - d, with a long after one register and after four
.method public static mix(IJIJ)J
    .registers 10
    int-to-long v0, p0
    add-long/2addr v0, p1
    int-to-long v2, p3
    mul-long/2addr v0, v2
    sub-long/2addr v0, p4
    return-wide v0
.end method

.method public static twice(J)J
    .registers 4
    add-long v0, p0, p0
    return-wide v0
.end method

# Every form of move-wide, with pairs that overlap each way, registers above 255, and the 16-bit moves of
# ints and objects on the way. Returns x + 0x55 + 4.
.method public static moves(J)J
    .registers 300
    move-wide/from16 v0, p0
    move-wide v2, v0
    move-wide v3, v2
    move-wide v2, v3
    move-wide/16 v280, v2
    move-wide/from16 v10, v280
    const/16 v20, 0x55
    move/16 v290, v20
    move/from16 v5, v290
    int-to-long v6, v5
    add-long v0, v10, v6
    const-string v20, "wide"
    move-object/16 v291, v20
    move-object/from16 v7, v291
    invoke-virtual {v7}, Ljava/lang/String;->length()I
    move-result v5
    int-to-long v6, v5
    add-long/2addr v0, v6
    return-wide v0
.end method

# Static and instance fields of two registers, with an int field between them. Returns j + d + 123.
.method public static fields(JD)D
    .registers 8
    sput-wide p0, Lt/Wide;->sj:J
    sput-wide p2, Lt/Wide;->sd:D
    new-instance v0, Lt/Wide;
    invoke-direct {v0}, Lt/Wide;-><init>()V
    sget-wide v2, Lt/Wide;->sj:J
    iput-wide v2, v0, Lt/Wide;->j:J
    sget-wide v2, Lt/Wide;->sd:D
    iput-wide v2, v0, Lt/Wide;->d:D
    const/16 v1, 0x7b
    iput v1, v0, Lt/Wide;->i:I
    iget-wide v2, v0, Lt/Wide;->j:J
    long-to-double v2, v2
    iget-wide p0, v0, Lt/Wide;->d:D
    add-double/2addr v2, p0
    iget v1, v0, Lt/Wide;->i:I
    int-to-double p0, v1
    add-double/2addr v2, p0
    return-wide v2
.end method

# A call with wide arguments by range, a wide result, and a call that names two registers for one long.
# Returns ((3 + x) * 5 - 100) * 2.
.method public static calls(J)J
    .registers 12
    const/4 v0, 0x3
    const/4 v3, 0x5
    const-wide/16 v4, 0x64
    move-wide v1, p0
    invoke-static/range {v0 .. v5}, Lt/Wide;->mix(IJIJ)J
    move-result-wide v6
    invoke-static {v6, v7}, Lt/Wide;->twice(J)J
    move-result-wide v6
    return-wide v6
.end method

.method public scaled(DJ)D
    .registers 7
    long-to-double v0, p3
    mul-double/2addr v0, p1
    iget-wide p1, p0, Lt/Wide;->d:D
    add-double/2addr v0, p1
    return-wide v0
.end method

# A virtual call that fills all five registers an invoke can name: the object, a double, a long.
# Returns j * d + 1.0.
.method public static five(DJ)D
    .registers 9
    new-instance v0, Lt/Wide;
    invoke-direct {v0}, Lt/Wide;-><init>()V
    const-wide/high16 v1, 0x3ff0000000000000L
    iput-wide v1, v0, Lt/Wide;->d:D
    move-wide v1, p0
    move-wide v3, p2
    invoke-virtual {v0, v1, v2, v3, v4}, Lt/Wide;->scaled(DJ)D
    move-result-wide v1
    return-wide v1
.end method
