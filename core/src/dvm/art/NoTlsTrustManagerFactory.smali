# meglet.security.NoTlsTrustManagerFactory: TrustManagerFactory "PKIX" (and its other name, "X509") while there is no TLS
# (NoTlsContext.smali says why there are such classes at all).
#
# An HTTP client of an app's own -- OkHttp 3 and 4, Amazon's MAP library in MX Player -- asks for the platform's trust
# managers before it asks for an SSLContext, and without this factory its first https request, or the static
# initialiser of the class that makes the client, failed with NoSuchAlgorithmException "PKIX TrustManagerFactory not
# available" (in MX Player: "Failed to initialize SSLContext", and the class unusable from then on). init takes anything;
# the one trust manager trusts no certificate (NoTlsTrustManager.smali), and no connection gets that far anyway.
.class public final Lmeglet/security/NoTlsTrustManagerFactory;
.super Ljavax/net/ssl/TrustManagerFactorySpi;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljavax/net/ssl/TrustManagerFactorySpi;-><init>()V
    return-void
.end method

.method protected engineInit(Ljava/security/KeyStore;)V
    .registers 2
    return-void
.end method

.method protected engineInit(Ljavax/net/ssl/ManagerFactoryParameters;)V
    .registers 2
    return-void
.end method

.method protected engineGetTrustManagers()[Ljavax/net/ssl/TrustManager;
    .registers 4
    const/4 v0, 0x1
    new-array v0, v0, [Ljavax/net/ssl/TrustManager;
    new-instance v1, Lmeglet/security/NoTlsTrustManager;
    invoke-direct {v1}, Lmeglet/security/NoTlsTrustManager;-><init>()V
    const/4 v2, 0x0
    aput-object v1, v0, v2
    return-object v0
.end method
