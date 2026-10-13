# java.lang.Number. The boxes that extend it are generated (tools/dvm-boot.py).
.class public abstract Ljava/lang/Number;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public abstract intValue()I
.end method

.method public abstract longValue()J
.end method

.method public abstract floatValue()F
.end method

.method public abstract doubleValue()D
.end method

.method public byteValue()B
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Number;->intValue()I
    move-result v0
    int-to-byte v0, v0
    return v0
.end method

.method public shortValue()S
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Number;->intValue()I
    move-result v0
    int-to-short v0, v0
    return v0
.end method
