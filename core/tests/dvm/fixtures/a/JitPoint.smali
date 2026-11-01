# The fields t.Jit.fields reads and writes in its compiled loop: one of each width, an object, and two static ones.
.class public final Lt/JitPoint;
.super Ljava/lang/Object;

.field public i:I
.field public j:J
.field public b:B
.field public c:C
.field public s:S
.field public z:Z
.field public o:Ljava/lang/Object;
.field public static total:I
.field public static wide:J

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
