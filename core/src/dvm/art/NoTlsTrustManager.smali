# megler.security.NoTlsTrustManager: the trust manager of NoTlsTrustManagerFactory.smali. It trusts no certificate -- a
# CertificateException, "no TLS on this phone yet" -- and accepts no issuer, which is all a trust manager can honestly say
# where no connection can be secured.
.class public final Lmegler/security/NoTlsTrustManager;
.super Ljava/lang/Object;
.implements Ljavax/net/ssl/X509TrustManager;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method private static refused()Ljava/security/cert/CertificateException;
    .registers 2
    new-instance v0, Ljava/security/cert/CertificateException;
    const-string v1, "no TLS on this phone yet"
    invoke-direct {v0, v1}, Ljava/security/cert/CertificateException;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

.method public checkClientTrusted([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V
    .registers 4
    invoke-static {}, Lmegler/security/NoTlsTrustManager;->refused()Ljava/security/cert/CertificateException;
    move-result-object v0
    throw v0
.end method

.method public checkServerTrusted([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V
    .registers 4
    invoke-static {}, Lmegler/security/NoTlsTrustManager;->refused()Ljava/security/cert/CertificateException;
    move-result-object v0
    throw v0
.end method

.method public getAcceptedIssuers()[Ljava/security/cert/X509Certificate;
    .registers 2
    const/4 v0, 0x0
    new-array v0, v0, [Ljava/security/cert/X509Certificate;
    return-object v0
.end method
