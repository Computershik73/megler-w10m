# java.lang.Throwable. The three fields have libcore's names, which is what the runtime looks them up by
# (mg_dvm_describe_pending, mg_dvm_pending_stack): `backtrace` holds a long[] of (method, pc) pairs.
.class public Ljava/lang/Throwable;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;

.field private detailMessage:Ljava/lang/String;
.field private cause:Ljava/lang/Throwable;
.field private backtrace:Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    invoke-virtual {p0}, Ljava/lang/Throwable;->fillInStackTrace()Ljava/lang/Throwable;
    return-void
.end method

.method public constructor <init>(Ljava/lang/String;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Ljava/lang/Throwable;->detailMessage:Ljava/lang/String;
    invoke-virtual {p0}, Ljava/lang/Throwable;->fillInStackTrace()Ljava/lang/Throwable;
    return-void
.end method

.method public constructor <init>(Ljava/lang/String;Ljava/lang/Throwable;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Ljava/lang/Throwable;->detailMessage:Ljava/lang/String;
    iput-object p2, p0, Ljava/lang/Throwable;->cause:Ljava/lang/Throwable;
    invoke-virtual {p0}, Ljava/lang/Throwable;->fillInStackTrace()Ljava/lang/Throwable;
    return-void
.end method

# The message of an exception made from its cause alone is the cause's toString(), as in Java.
.method public constructor <init>(Ljava/lang/Throwable;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Ljava/lang/Throwable;->cause:Ljava/lang/Throwable;
    if-eqz p1, :done
    invoke-virtual {p1}, Ljava/lang/Object;->toString()Ljava/lang/String;
    move-result-object v0
    iput-object v0, p0, Ljava/lang/Throwable;->detailMessage:Ljava/lang/String;
    :done
    invoke-virtual {p0}, Ljava/lang/Throwable;->fillInStackTrace()Ljava/lang/Throwable;
    return-void
.end method

.method public getMessage()Ljava/lang/String;
    .registers 2
    iget-object v0, p0, Ljava/lang/Throwable;->detailMessage:Ljava/lang/String;
    return-object v0
.end method

.method public getLocalizedMessage()Ljava/lang/String;
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public getCause()Ljava/lang/Throwable;
    .registers 2
    iget-object v0, p0, Ljava/lang/Throwable;->cause:Ljava/lang/Throwable;
    return-object v0
.end method

.method public initCause(Ljava/lang/Throwable;)Ljava/lang/Throwable;
    .registers 2
    iput-object p1, p0, Ljava/lang/Throwable;->cause:Ljava/lang/Throwable;
    return-object p0
.end method

.method public native fillInStackTrace()Ljava/lang/Throwable;
.end method

.method public native toString()Ljava/lang/String;
.end method

.method public native printStackTrace()V
.end method

.method public final addSuppressed(Ljava/lang/Throwable;)V
    .registers 2
    return-void
.end method
