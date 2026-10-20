# megler.security.NoTlsContext: SSLContext "TLS", "Default", "TLSv1.2" and their kin while there is no TLS -- while
# conscrypt's native library is not in the Android root (mg_dvm_art.c, "the security providers").
#
# Why it is there at all. Without any SSLContext an app that opens an https URL does not get a failed connection: Android's
# own HTTP client (com.android.okhttp, behind URL.openConnection) asks SSLContext.getInstance("TLS"), and when that throws
# it throws AssertionError ("The system has no TLS. Just give up."), which ends the thread that asked. Firebase's threads in
# MX Player and an ad SDK's in Super S22 Launcher died that way. With this context the client gets its socket factory
# (NoTlsSocketFactory.smali), and the connection fails where a connection fails, with an IOException.
#
# init() takes anything and does nothing; there are no sessions to keep; an SSLEngine or a server socket factory is
# UnsupportedOperationException, saying why.
.class public final Lmegler/security/NoTlsContext;
.super Ljavax/net/ssl/SSLContextSpi;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljavax/net/ssl/SSLContextSpi;-><init>()V
    return-void
.end method

.method protected engineInit([Ljavax/net/ssl/KeyManager;[Ljavax/net/ssl/TrustManager;Ljava/security/SecureRandom;)V
    .registers 4
    return-void
.end method

.method protected engineGetSocketFactory()Ljavax/net/ssl/SSLSocketFactory;
    .registers 2
    new-instance v0, Lmegler/security/NoTlsSocketFactory;
    invoke-direct {v0}, Lmegler/security/NoTlsSocketFactory;-><init>()V
    return-object v0
.end method

.method private static unsupported()Ljava/lang/UnsupportedOperationException;
    .registers 2
    new-instance v0, Ljava/lang/UnsupportedOperationException;
    const-string v1, "no TLS on this phone yet"
    invoke-direct {v0, v1}, Ljava/lang/UnsupportedOperationException;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

.method protected engineGetServerSocketFactory()Ljavax/net/ssl/SSLServerSocketFactory;
    .registers 2
    invoke-static {}, Lmegler/security/NoTlsContext;->unsupported()Ljava/lang/UnsupportedOperationException;
    move-result-object v0
    throw v0
.end method

.method protected engineCreateSSLEngine()Ljavax/net/ssl/SSLEngine;
    .registers 2
    invoke-static {}, Lmegler/security/NoTlsContext;->unsupported()Ljava/lang/UnsupportedOperationException;
    move-result-object v0
    throw v0
.end method

.method protected engineCreateSSLEngine(Ljava/lang/String;I)Ljavax/net/ssl/SSLEngine;
    .registers 4
    invoke-static {}, Lmegler/security/NoTlsContext;->unsupported()Ljava/lang/UnsupportedOperationException;
    move-result-object v0
    throw v0
.end method

.method protected engineGetServerSessionContext()Ljavax/net/ssl/SSLSessionContext;
    .registers 2
    const/4 v0, 0x0
    return-object v0
.end method

.method protected engineGetClientSessionContext()Ljavax/net/ssl/SSLSessionContext;
    .registers 2
    const/4 v0, 0x0
    return-object v0
.end method
