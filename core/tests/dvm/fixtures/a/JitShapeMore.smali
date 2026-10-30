# A JitShape whose getI is not trivial and overrides the one compiled code does in place: its calls go by mg_dvm_jit_invoke.
.class public final Lt/JitShapeMore;
.super Lt/JitShape;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Lt/JitShape;-><init>()V
    return-void
.end method

.method public getI()I
    .registers 2
    iget v0, p0, Lt/JitShape;->i:I
    add-int/lit8 v0, v0, 0x64
    return v0
.end method
