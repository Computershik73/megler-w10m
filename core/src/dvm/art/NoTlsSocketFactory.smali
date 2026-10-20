# meglet.security.NoTlsSocketFactory: the socket factory of NoTlsContext.smali, and so SSLSocketFactory.getDefault()'s and
# HttpsURLConnection's default while there is no TLS.
#
# Every socket it is asked for is a connection that cannot be had: an SSLException ("no TLS on this phone yet"), which is
# an IOException, from the call that would connect or would lay TLS over a connected socket. That is where a connection
# to a name that does not resolve fails too, and what an app is written to survive. It offers no cipher suites.
.class public final Lmeglet/security/NoTlsSocketFactory;
.super Ljavax/net/ssl/SSLSocketFactory;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljavax/net/ssl/SSLSocketFactory;-><init>()V
    return-void
.end method

.method public getDefaultCipherSuites()[Ljava/lang/String;
    .registers 2
    const/4 v0, 0x0
    new-array v0, v0, [Ljava/lang/String;
    return-object v0
.end method

.method public getSupportedCipherSuites()[Ljava/lang/String;
    .registers 2
    const/4 v0, 0x0
    new-array v0, v0, [Ljava/lang/String;
    return-object v0
.end method

.method private static refused()Ljavax/net/ssl/SSLException;
    .registers 2
    new-instance v0, Ljavax/net/ssl/SSLException;
    const-string v1, "no TLS on this phone yet"
    invoke-direct {v0, v1}, Ljavax/net/ssl/SSLException;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

.method public createSocket()Ljava/net/Socket;
    .registers 2
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method

.method public createSocket(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;
    .registers 6
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method

.method public createSocket(Ljava/lang/String;I)Ljava/net/Socket;
    .registers 4
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method

.method public createSocket(Ljava/lang/String;ILjava/net/InetAddress;I)Ljava/net/Socket;
    .registers 6
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method

.method public createSocket(Ljava/net/InetAddress;I)Ljava/net/Socket;
    .registers 4
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method

.method public createSocket(Ljava/net/InetAddress;ILjava/net/InetAddress;I)Ljava/net/Socket;
    .registers 6
    invoke-static {}, Lmeglet/security/NoTlsSocketFactory;->refused()Ljavax/net/ssl/SSLException;
    move-result-object v0
    throw v0
.end method
