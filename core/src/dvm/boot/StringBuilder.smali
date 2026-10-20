# java.lang.StringBuilder: what d8 turns string concatenation into. The text is a char[] and a count;
# the appends are native (mg_dvm_boot.c) and grow the array.
.class public final Ljava/lang/StringBuilder;
.super Ljava/lang/Object;
.implements Ljava/lang/CharSequence;
.implements Ljava/io/Serializable;

.field private value:[C
.field private count:I

.method public constructor <init>()V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const/16 v0, 0x10
    new-array v0, v0, [C
    iput-object v0, p0, Ljava/lang/StringBuilder;->value:[C
    return-void
.end method

.method public constructor <init>(I)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    new-array v0, p1, [C
    iput-object v0, p0, Ljava/lang/StringBuilder;->value:[C
    return-void
.end method

.method public constructor <init>(Ljava/lang/String;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/StringBuilder;-><init>()V
    invoke-virtual {p0, p1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    return-void
.end method

.method public native append(Ljava/lang/String;)Ljava/lang/StringBuilder;
.end method

.method public native append(C)Ljava/lang/StringBuilder;
.end method

.method public native append(Z)Ljava/lang/StringBuilder;
.end method

.method public native append(I)Ljava/lang/StringBuilder;
.end method

.method public native append(J)Ljava/lang/StringBuilder;
.end method

.method public native append(F)Ljava/lang/StringBuilder;
.end method

.method public native append(D)Ljava/lang/StringBuilder;
.end method

.method public native append([C)Ljava/lang/StringBuilder;
.end method

.method public append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    .registers 3
    invoke-static {p1}, Ljava/lang/String;->valueOf(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    invoke-virtual {p0, v0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    return-object p0
.end method

.method public append(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;
    .registers 3
    invoke-static {p1}, Ljava/lang/String;->valueOf(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    invoke-virtual {p0, v0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    return-object p0
.end method

.method public length()I
    .registers 2
    iget v0, p0, Ljava/lang/StringBuilder;->count:I
    return v0
.end method

.method public native charAt(I)C
.end method

.method public native setLength(I)V
.end method

.method public native toString()Ljava/lang/String;
.end method

.method public subSequence(II)Ljava/lang/CharSequence;
    .registers 4
    invoke-virtual {p0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v0, p1, p2}, Ljava/lang/String;->substring(II)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
