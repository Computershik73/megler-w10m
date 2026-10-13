# Monitors, as a compiler writes them. A synchronized method in a DEX file is not a flag the runtime acts on
# (that flag is only valid on native methods): it is monitor-enter at the start, monitor-exit before each
# return, and a handler for everything that exits and throws on. So that is what is here.
.class public final Lt/Sync;
.super Ljava/lang/Object;

.field public count:I
.field public static scount:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public declared-synchronized bump()I
    .registers 3
    monitor-enter p0
    :try_start
    iget v0, p0, Lt/Sync;->count:I
    add-int/lit8 v0, v0, 0x1
    iput v0, p0, Lt/Sync;->count:I
    :try_end
    .catchall {:try_start .. :try_end} :handler
    monitor-exit p0
    return v0
    :handler
    move-exception v1
    monitor-exit p0
    throw v1
.end method

.method public bumpMany(I)V
    .registers 3
    :loop
    if-lez p1, :done
    invoke-virtual {p0}, Lt/Sync;->bump()I
    add-int/lit8 p1, p1, -0x1
    goto :loop
    :done
    return-void
.end method

# A synchronized method calling another on the same object: the lock is taken twice by its owner.
.method public declared-synchronized twice()I
    .registers 3
    monitor-enter p0
    :try_start
    invoke-virtual {p0}, Lt/Sync;->bump()I
    invoke-virtual {p0}, Lt/Sync;->bump()I
    move-result v0
    :try_end
    .catchall {:try_start .. :try_end} :handler
    monitor-exit p0
    return v0
    :handler
    move-exception v1
    monitor-exit p0
    throw v1
.end method

# Thrown with the lock held: the handler lets go of it on the way out.
.method public declared-synchronized throwing()V
    .registers 3
    monitor-enter p0
    :try_start
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "thrown with the lock held"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :try_end
    .catchall {:try_start .. :try_end} :handler
    :handler
    move-exception v1
    monitor-exit p0
    throw v1
.end method

# A static synchronized method locks its class.
.method public static declared-synchronized sbump()I
    .registers 3
    const-class v2, Lt/Sync;
    monitor-enter v2
    :try_start
    sget v0, Lt/Sync;->scount:I
    add-int/lit8 v0, v0, 0x1
    sput v0, Lt/Sync;->scount:I
    :try_end
    .catchall {:try_start .. :try_end} :handler
    monitor-exit v2
    return v0
    :handler
    move-exception v1
    monitor-exit v2
    throw v1
.end method

.method public static badExit(Ljava/lang/Object;)V
    .registers 1
    monitor-exit p0
    return-void
.end method

.method public static badNotify(Ljava/lang/Object;)V
    .registers 1
    invoke-virtual {p0}, Ljava/lang/Object;->notify()V
    return-void
.end method

.method public static badWait(Ljava/lang/Object;)V
    .registers 1
    invoke-virtual {p0}, Ljava/lang/Object;->wait()V
    return-void
.end method

# notify() and a timed wait() by the lock's owner: nobody else is there, so the wait ends when its time is up.
.method public static goodWait(Ljava/lang/Object;)V
    .registers 3
    monitor-enter p0
    invoke-virtual {p0}, Ljava/lang/Object;->notifyAll()V
    const-wide/16 v0, 0x5
    invoke-virtual {p0, v0, v1}, Ljava/lang/Object;->wait(J)V
    monitor-exit p0
    return-void
.end method
