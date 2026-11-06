# Exceptions: caught where thrown, by a superclass, by a catch-all; through several frames with finally
# blocks on the way; thrown by the interpreter itself, by a native method, by a handler.
.class public final Lt/Exc;
.super Ljava/lang/Object;
.source "Exc.java"

.field public static log:Ljava/lang/StringBuilder;
.field public value:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

# a / b, or -1 when b is zero
.method public static divide(II)I
    .registers 3
    :try_start
    div-int v0, p0, p1
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :handler
    return v0
    :handler
    move-exception v0
    const/4 v0, -0x1
    return v0
.end method

# A handler for another class does not catch: the exception leaves the method.
.method public static wrongCatch(II)I
    .registers 3
    :try_start
    div-int v0, p0, p1
    :try_end
    .catch Ljava/lang/NullPointerException; {:try_start .. :try_end} :handler
    return v0
    :handler
    const/4 v0, -0x1
    return v0
.end method

# Handlers are tried in order: 0 throws what the first catches, 1 what the second (its superclass) does,
# 2 what only the catch-all does, anything else nothing. Returns which handler ran, 0 for none.
.method public static which(I)I
    .registers 3
    :try_start
    if-nez p0, :not_zero
    const/4 v0, 0x0
    invoke-virtual {v0}, Ljava/lang/Object;->hashCode()I
    :not_zero
    const/4 v0, 0x1
    if-ne p0, v0, :not_one
    const/4 v1, 0x0
    div-int/2addr v0, v1
    :not_one
    const/4 v0, 0x2
    if-ne p0, v0, :not_two
    new-instance v0, Ljava/lang/Error;
    invoke-direct {v0}, Ljava/lang/Error;-><init>()V
    throw v0
    :not_two
    :try_end
    .catch Ljava/lang/NullPointerException; {:try_start .. :try_end} :first
    .catch Ljava/lang/RuntimeException; {:try_start .. :try_end} :second
    .catchall {:try_start .. :try_end} :third
    const/4 v0, 0x0
    return v0
    :first
    const/4 v0, 0x1
    return v0
    :second
    const/4 v0, 0x2
    return v0
    :third
    const/4 v0, 0x3
    return v0
.end method

.method private static note(Ljava/lang/String;)V
    .registers 2
    sget-object v0, Lt/Exc;->log:Ljava/lang/StringBuilder;
    invoke-virtual {v0, p0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    return-void
.end method

.method public static level3(I)V
    .registers 3
    const-string v0, "3"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    if-eqz p0, :quiet
    new-instance v0, Lt/MyExc;
    const-string v1, "deep"
    .line 30
    invoke-direct {v0, v1, p0}, Lt/MyExc;-><init>(Ljava/lang/String;I)V
    .line 31
    throw v0
    :quiet
    return-void
.end method

# try { note("2"); level3(n); note("x"); } finally { note("f"); }
.method public static level2(I)V
    .registers 3
    :try_start
    const-string v0, "2"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    .line 40
    invoke-static {p0}, Lt/Exc;->level3(I)V
    const-string v0, "x"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    :try_end
    .catchall {:try_start .. :try_end} :finally
    const-string v0, "f"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    return-void
    :finally
    move-exception v1
    const-string v0, "f"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    .line 44
    throw v1
.end method

# try { level2(n); return 0; } catch (MyExc e) { note("c"); return e.code; }
.method public static level1(I)I
    .registers 3
    :try_start
    .line 50
    invoke-static {p0}, Lt/Exc;->level2(I)V
    :try_end
    .catch Lt/MyExc; {:try_start .. :try_end} :handler
    const/4 v0, 0x0
    return v0
    :handler
    move-exception v1
    const-string v0, "c"
    invoke-static {v0}, Lt/Exc;->note(Ljava/lang/String;)V
    iget v0, v1, Lt/MyExc;->code:I
    return v0
.end method

# What happened, in order, for level1(n): "23xf0" when nothing is thrown, "23fc" and the code when it is.
.method public static trace(I)Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    sput-object v0, Lt/Exc;->log:Ljava/lang/StringBuilder;
    invoke-static {p0}, Lt/Exc;->level1(I)I
    move-result v1
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# The same without the catch: the exception reaches whoever called from C, finally blocks run on the way.
.method public static uncaught(I)Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    sput-object v0, Lt/Exc;->log:Ljava/lang/StringBuilder;
    .line 70
    invoke-static {p0}, Lt/Exc;->level2(I)V
    const/4 v0, 0x0
    return-object v0
.end method

.method public static logged()Ljava/lang/String;
    .registers 1
    sget-object v0, Lt/Exc;->log:Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# A try inside a try: the inner one does not catch a division by zero, the outer one does.
# 100 / n + 1, or 999.
.method public static nested(I)I
    .registers 4
    const/4 v0, 0x0
    :outer_start
    :inner_start
    const/16 v1, 0x64
    div-int/2addr v1, p0
    add-int/2addr v0, v1
    :inner_end
    .catch Ljava/lang/NullPointerException; {:inner_start .. :inner_end} :inner_handler
    goto :after
    :inner_handler
    move-exception v1
    const/4 v0, -0x5
    :after
    add-int/lit8 v0, v0, 0x1
    :outer_end
    .catch Ljava/lang/ArithmeticException; {:outer_start .. :outer_end} :outer_handler
    return v0
    :outer_handler
    move-exception v1
    const/16 v0, 0x3e7
    return v0
.end method

# An exception thrown inside a handler is caught by the try around it. Returns 2.
.method public static inHandler()I
    .registers 3
    :outer_start
    :inner_start
    const/4 v0, 0x0
    invoke-virtual {v0}, Ljava/lang/Object;->hashCode()I
    :inner_end
    .catch Ljava/lang/NullPointerException; {:inner_start .. :inner_end} :first
    const/4 v0, 0x0
    return v0
    :first
    move-exception v1
    const/4 v2, 0x0
    div-int v2, v2, v2
    const/4 v0, 0x1
    return v0
    :outer_end
    .catch Ljava/lang/ArithmeticException; {:outer_start .. :outer_end} :second
    :second
    move-exception v1
    const/4 v0, 0x2
    return v0
.end method

.method public static throwNull()V
    .registers 1
    const/4 v0, 0x0
    throw v0
.end method

# s.charAt(i), or -1: the exception is thrown by a native method and caught here by its superclass.
.method public static charAt(Ljava/lang/String;I)I
    .registers 3
    :try_start
    invoke-virtual {p0, p1}, Ljava/lang/String;->charAt(I)C
    move-result v0
    :try_end
    .catch Ljava/lang/IndexOutOfBoundsException; {:try_start .. :try_end} :handler
    return v0
    :handler
    const/4 v0, -0x1
    return v0
.end method

# The message of an exception made and caught here.
.method public static message()Ljava/lang/String;
    .registers 3
    :try_start
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "boom"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :handler
    :handler
    move-exception v0
    invoke-virtual {v0}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# catch (ArithmeticException e) { throw new RuntimeException("wrapped", e); }
.method public static caused()V
    .registers 4
    :try_start
    const/4 v0, 0x1
    const/4 v1, 0x0
    div-int/2addr v0, v1
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :handler
    return-void
    :handler
    move-exception v0
    new-instance v1, Ljava/lang/RuntimeException;
    const-string v2, "wrapped"
    invoke-direct {v1, v2, v0}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;Ljava/lang/Throwable;)V
    throw v1
.end method

.method public static cast(Ljava/lang/Object;)Ljava/lang/String;
    .registers 1
    check-cast p0, Ljava/lang/String;
    return-object p0
.end method

.method public static field(Lt/Exc;)I
    .registers 2
    iget v0, p0, Lt/Exc;->value:I
    return v0
.end method

.method public static store(Lt/Exc;)V
    .registers 2
    const/4 v0, 0x5
    iput v0, p0, Lt/Exc;->value:I
    return-void
.end method

.method public static longDivide(JJ)J
    .registers 6
    div-long v0, p0, p2
    return-wide v0
.end method

# A class whose static initialiser throws.
.method public static boom()I
    .registers 1
    invoke-static {}, Lt/Boom;->touch()I
    move-result v0
    return v0
.end method

# level2's exception caught and made the cause of a Throwable of its own, which reaches C from outer: the wrapper's frames,
# then "Caused by" and the cause's, the frame both have at the bottom (outer's call, line 90) said as "... 1 more".
.method public static wrapped(I)V
    .registers 4
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    sput-object v0, Lt/Exc;->log:Ljava/lang/StringBuilder;
    :try_start
    .line 80
    invoke-static {p0}, Lt/Exc;->level2(I)V
    :try_end
    .catch Lt/MyExc; {:try_start .. :try_end} :caught
    return-void
    :caught
    move-exception v0
    new-instance v1, Ljava/lang/Throwable;
    const-string v2, "wrapped"
    .line 82
    invoke-direct {v1, v2, v0}, Ljava/lang/Throwable;-><init>(Ljava/lang/String;Ljava/lang/Throwable;)V
    throw v1
.end method

.method public static outer(I)V
    .registers 1
    .line 90
    invoke-static {p0}, Lt/Exc;->wrapped(I)V
    return-void
.end method
