# A subclass whose scale() the library must reach when it calls Guest.scale on one of these.
.class public final Lt/GuestSub;
.super Lt/Guest;

.method public constructor <init>(I)V
    .registers 2
    invoke-direct {p0, p1}, Lt/Guest;-><init>(I)V
    return-void
.end method

.method public scale(I)I
    .registers 3
    invoke-super {p0, p1}, Lt/Guest;->scale(I)I
    move-result v0
    add-int/lit16 v0, v0, 0x3e8
    return v0
.end method
