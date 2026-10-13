# java.lang.Boolean: two objects, made when the class is initialised.
.class public final Ljava/lang/Boolean;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;

.field public static final TRUE:Ljava/lang/Boolean;
.field public static final FALSE:Ljava/lang/Boolean;
.field private final value:Z

.method static constructor <clinit>()V
    .registers 2
    new-instance v0, Ljava/lang/Boolean;
    const/4 v1, 0x1
    invoke-direct {v0, v1}, Ljava/lang/Boolean;-><init>(Z)V
    sput-object v0, Ljava/lang/Boolean;->TRUE:Ljava/lang/Boolean;
    new-instance v0, Ljava/lang/Boolean;
    const/4 v1, 0x0
    invoke-direct {v0, v1}, Ljava/lang/Boolean;-><init>(Z)V
    sput-object v0, Ljava/lang/Boolean;->FALSE:Ljava/lang/Boolean;
    return-void
.end method

.method public constructor <init>(Z)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-boolean p1, p0, Ljava/lang/Boolean;->value:Z
    return-void
.end method

.method public static valueOf(Z)Ljava/lang/Boolean;
    .registers 2
    if-eqz p0, :no
    sget-object v0, Ljava/lang/Boolean;->TRUE:Ljava/lang/Boolean;
    return-object v0
    :no
    sget-object v0, Ljava/lang/Boolean;->FALSE:Ljava/lang/Boolean;
    return-object v0
.end method

.method public booleanValue()Z
    .registers 2
    iget-boolean v0, p0, Ljava/lang/Boolean;->value:Z
    return v0
.end method

.method public hashCode()I
    .registers 2
    iget-boolean v0, p0, Ljava/lang/Boolean;->value:Z
    if-eqz v0, :no
    const/16 v0, 0x4cf
    return v0
    :no
    const/16 v0, 0x4d5
    return v0
.end method

.method public equals(Ljava/lang/Object;)Z
    .registers 4
    instance-of v0, p1, Ljava/lang/Boolean;
    if-eqz v0, :no
    check-cast p1, Ljava/lang/Boolean;
    iget-boolean v0, p0, Ljava/lang/Boolean;->value:Z
    iget-boolean v1, p1, Ljava/lang/Boolean;->value:Z
    if-ne v0, v1, :no
    const/4 v0, 0x1
    return v0
    :no
    const/4 v0, 0x0
    return v0
.end method

.method public toString()Ljava/lang/String;
    .registers 2
    iget-boolean v0, p0, Ljava/lang/Boolean;->value:Z
    invoke-static {v0}, Ljava/lang/String;->valueOf(Z)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
