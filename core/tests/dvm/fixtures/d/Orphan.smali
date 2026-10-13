# A class whose superclass nothing defines, and one that needs it.
.class public Lt/Orphan;
.super Lno/such/Parent;

.method public static touch()I
    .registers 1
    const/4 v0, 0x1
    return v0
.end method
