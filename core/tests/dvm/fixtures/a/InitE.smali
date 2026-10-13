# Static fields whose values are in the file: they are there before the static initialiser runs.
.class public final Lt/InitE;
.super Ljava/lang/Object;

.field public static final Z:Z = true
.field public static final B:B = -0x5
.field public static final S:S = 0x1234
.field public static final C:C = 'x'
.field public static final I:I = -0x12345678
.field public static final J:J = 0x123456789abcdefL
.field public static final F:F = 1.5f
.field public static final D:D = -2.5
.field public static final STR:Ljava/lang/String; = "preset"
.field public static final NUL:Ljava/lang/String; = null
.field public static final SMALL:J = 0x7fL
.field public static final WHOLE:D = 3.0
.field public static seen:I
.field public static plain:I

.method static constructor <clinit>()V
    .registers 2
    sget v0, Lt/InitE;->I:I
    sget-short v1, Lt/InitE;->S:S
    add-int/2addr v0, v1
    sput v0, Lt/InitE;->seen:I
    return-void
.end method
