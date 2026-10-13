# An exception class of the app's own, with a field.
.class public Lt/MyExc;
.super Ljava/lang/Exception;
.source "MyExc.java"

.field public final code:I

.method public constructor <init>(Ljava/lang/String;I)V
    .registers 3
    invoke-direct {p0, p1}, Ljava/lang/Exception;-><init>(Ljava/lang/String;)V
    iput p2, p0, Lt/MyExc;->code:I
    return-void
.end method
