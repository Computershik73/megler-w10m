# meglet.security.RuntimeProvider: what stands in conscrypt's place in the list of security providers while conscrypt's native
# library is not there (mg_dvm_art.c, "the security providers").
#
# It goes by conscrypt's own name, "AndroidOpenSSL", because that name is asked for: BouncyCastle's own key derivation and
# signatures take their digests from the provider of that name and refuse to start on Android without it, and libcore's
# strongest random generator is "SHA1PRNG:AndroidOpenSSL". What it offers under the name is the little that can be had
# without BoringSSL: random bytes from the system (SystemRandom.smali), and the six digests, which are BouncyCastle's own
# classes written in Java. And an SSLContext of every name Android 9's has (TLS, Default, SSL, TLSv1 to TLSv1.2) whose
# connections fail politely (NoTlsContext.smali), and a TrustManagerFactory "PKIX" that trusts nothing
# (NoTlsTrustManagerFactory.smali): there is no TLS, and an app asking for it must get a failed connection, not the
# AssertionError Android's HTTP client throws when it finds no SSLContext at all, nor the NoSuchAlgorithmException an app's
# own client fails its start on. Anything else asked of "AndroidOpenSSL" is NoSuchAlgorithmException.
.class public final Lmeglet/security/RuntimeProvider;
.super Ljava/security/Provider;

.method public constructor <init>()V
    .registers 5
    const-string v0, "AndroidOpenSSL"
    const-wide/high16 v1, 0x3ff0000000000000L
    const-string v3, "Meglet's stand-in for conscrypt: random numbers from the system, digests from BouncyCastle; no ciphers, no TLS"
    invoke-direct {p0, v0, v1, v2, v3}, Ljava/security/Provider;-><init>(Ljava/lang/String;DLjava/lang/String;)V
    const-string v0, "SecureRandom.SHA1PRNG"
    const-string v1, "meglet.security.SystemRandom"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.MD5"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.MD5$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.SHA-1"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.SHA1$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "Alg.Alias.MessageDigest.SHA1"
    const-string v1, "SHA-1"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.SHA-224"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.SHA224$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.SHA-256"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.SHA256$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.SHA-384"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.SHA384$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "MessageDigest.SHA-512"
    const-string v1, "com.android.org.bouncycastle.jcajce.provider.digest.SHA512$Digest"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.TLS"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.Default"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.SSL"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.TLSv1"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.TLSv1.1"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "SSLContext.TLSv1.2"
    const-string v1, "meglet.security.NoTlsContext"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "TrustManagerFactory.PKIX"
    const-string v1, "meglet.security.NoTlsTrustManagerFactory"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v0, "Alg.Alias.TrustManagerFactory.X509"
    const-string v1, "PKIX"
    invoke-virtual {p0, v0, v1}, Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    return-void
.end method
