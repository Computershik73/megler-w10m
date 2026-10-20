# megler.security.SystemRandom: random bytes from the system's source (mg_random in the host layer: BCryptGenRandom, /dev/urandom).
# It is named SHA1PRNG because that is the name Android's own goes by, and code asks for it by name; there is no SHA-1 in it.
.class public final Lmegler/security/SystemRandom;
.super Ljava/security/SecureRandomSpi;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/security/SecureRandomSpi;-><init>()V
    return-void
.end method

.method protected native engineNextBytes([B)V
.end method

# The system's source is seeded by the system: a seed handed in is not needed, and not used.
.method protected engineSetSeed([B)V
    .registers 2
    return-void
.end method

.method protected engineGenerateSeed(I)[B
    .registers 3
    new-array v0, p1, [B
    invoke-virtual {p0, v0}, Lmegler/security/SystemRandom;->engineNextBytes([B)V
    return-object v0
.end method
