# The trivial methods t.Jit.inlined calls in its compiled loop, which the code does in place (mg_dvm_jit.c, inline_call):
# getters and setters of several widths, a constant, an empty method, a static getter. Not final: JitShapeMore overrides getI.
.class public Lt/JitShape;
.super Ljava/lang/Object;

.field public i:I
.field public j:J
.field public c:C
.field public o:Ljava/lang/Object;
.field public static total:I

.method public constructor <init>()V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const-wide/16 v0, 0x1234
    iput-wide v0, p0, Lt/JitShape;->j:J
    iput-object p0, p0, Lt/JitShape;->o:Ljava/lang/Object;
    return-void
.end method

.method public getI()I
    .registers 2
    iget v0, p0, Lt/JitShape;->i:I
    return v0
.end method

.method public setI(I)V
    .registers 2
    iput p1, p0, Lt/JitShape;->i:I
    return-void
.end method

.method public getC()C
    .registers 2
    iget-char v0, p0, Lt/JitShape;->c:C
    return v0
.end method

.method public setC(C)V
    .registers 2
    iput-char p1, p0, Lt/JitShape;->c:C
    return-void
.end method

.method public getJ()J
    .registers 3
    iget-wide v0, p0, Lt/JitShape;->j:J
    return-wide v0
.end method

.method public getO()Ljava/lang/Object;
    .registers 2
    iget-object v0, p0, Lt/JitShape;->o:Ljava/lang/Object;
    return-object v0
.end method

.method public seven()I
    .registers 2
    const/4 v0, 0x7
    return v0
.end method

.method public touch()V
    .registers 1
    return-void
.end method

.method public static total()I
    .registers 1
    sget v0, Lt/JitShape;->total:I
    return v0
.end method
