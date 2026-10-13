# An enum, as javac and d8 write one: the constants made in the static initialiser, values() and valueOf().
.class public final enum Lt/Color;
.super Ljava/lang/Enum;

.field public static final enum RED:Lt/Color;
.field public static final enum GREEN:Lt/Color;
.field private static final synthetic $VALUES:[Lt/Color;

.method static constructor <clinit>()V
    .registers 4
    new-instance v0, Lt/Color;
    const-string v1, "RED"
    const/4 v2, 0x0
    invoke-direct {v0, v1, v2}, Lt/Color;-><init>(Ljava/lang/String;I)V
    sput-object v0, Lt/Color;->RED:Lt/Color;
    new-instance v1, Lt/Color;
    const-string v2, "GREEN"
    const/4 v3, 0x1
    invoke-direct {v1, v2, v3}, Lt/Color;-><init>(Ljava/lang/String;I)V
    sput-object v1, Lt/Color;->GREEN:Lt/Color;
    filled-new-array {v0, v1}, [Lt/Color;
    move-result-object v0
    sput-object v0, Lt/Color;->$VALUES:[Lt/Color;
    return-void
.end method

.method private constructor <init>(Ljava/lang/String;I)V
    .registers 3
    invoke-direct {p0, p1, p2}, Ljava/lang/Enum;-><init>(Ljava/lang/String;I)V
    return-void
.end method

.method public static values()[Lt/Color;
    .registers 1
    sget-object v0, Lt/Color;->$VALUES:[Lt/Color;
    invoke-virtual {v0}, [Lt/Color;->clone()Ljava/lang/Object;
    move-result-object v0
    check-cast v0, [Lt/Color;
    return-object v0
.end method

.method public static valueOf(Ljava/lang/String;)Lt/Color;
    .registers 2
    const-class v0, Lt/Color;
    invoke-static {v0, p0}, Ljava/lang/Enum;->valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;
    move-result-object v0
    check-cast v0, Lt/Color;
    return-object v0
.end method
