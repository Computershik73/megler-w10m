// Overlay of core/framework/gen-src/android/net/LocalSocket.java (tools/build-framework.sh).
// It differs in being a socket: the generated stub connected nowhere and gave no streams (null), and an app that talks to a
// part of its own over a socket of AF_UNIX stopped at its first use -- VirtualHere's USB server, at its start, on an
// InputStreamReader of null.
//
// How. As Android's LocalSocketImpl, on Linux's calls, here libcore's android.system.Os by reflection (the stubs the framework
// is compiled against have no Os), which the core answers with sockets of the app's own (core/src/bionic/mg_net.c): socket,
// connect and bind to a UnixSocketAddress of the namespace's kind, read and write for the streams, shutdown, close. A socket
// connects only to one of the same app here; descriptors are not passed (getAncillaryFileDescriptors is null).
package android.net;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public class LocalSocket implements java.io.Closeable {
    public static final int SOCKET_DGRAM = 1;
    public static final int SOCKET_STREAM = 2;
    public static final int SOCKET_SEQPACKET = 3;

    private final int sockType;
    private FileDescriptor fd;
    private boolean bound, connected, closed, inputShutdown, outputShutdown;
    private LocalSocketAddress localAddress, remoteAddress;
    private int soTimeout, receiveBufferSize = 212992, sendBufferSize = 212992;
    private FileDescriptor[] toSend;
    private InputStream in;
    private OutputStream out;

    public LocalSocket() { this(SOCKET_STREAM); }

    public LocalSocket(int sockType) {
        if (sockType != SOCKET_DGRAM && sockType != SOCKET_STREAM && sockType != SOCKET_SEQPACKET) throw new IllegalArgumentException("unknown sockType");
        this.sockType = sockType;
    }

    /** A socket of AF_UNIX that is connected already. */
    public LocalSocket(FileDescriptor fd) {
        this(SOCKET_STREAM);
        this.fd = fd;
        connected = bound = true;
    }

    // ---- libcore's Os, by reflection

    static Object os(String name, Class<?>[] types, Object... args) throws IOException {
        try {
            Method m = Class.forName("android.system.Os").getMethod(name, types);
            return m.invoke(null, args);
        } catch (InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof IOException) throw (IOException) t;
            if (t instanceof RuntimeException) throw (RuntimeException) t;
            // (android.system.ErrnoException, as LocalSocketImpl turns it.)
            throw new IOException(t != null ? t.getMessage() : name, t);
        } catch (ReflectiveOperationException e) {
            throw new IOException("Os." + name + " is not there", e);
        }
    }

    static int constant(String name) {
        try {
            return Class.forName("android.system.OsConstants").getField(name).getInt(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("OsConstants." + name + " is not there", e);
        }
    }

    /** The address as libcore's UnixSocketAddress: abstract, a path, or a socket of /dev/socket. */
    static java.net.SocketAddress unixAddress(LocalSocketAddress a) throws IOException {
        try {
            Class<?> c = Class.forName("android.system.UnixSocketAddress");
            switch (a.getNamespace()) {
                case ABSTRACT: return (java.net.SocketAddress) c.getMethod("createAbstract", String.class).invoke(null, a.getName());
                case RESERVED: return (java.net.SocketAddress) c.getMethod("createFileSystem", String.class).invoke(null, "/dev/socket/" + a.getName());
                default: return (java.net.SocketAddress) c.getMethod("createFileSystem", String.class).invoke(null, a.getName());
            }
        } catch (ReflectiveOperationException e) {
            throw new IOException("no UnixSocketAddress for " + a.getName(), e);
        }
    }

    static FileDescriptor newSocket(int sockType) throws IOException {
        int type = sockType == SOCKET_DGRAM ? constant("SOCK_DGRAM") : sockType == SOCKET_SEQPACKET ? constant("SOCK_SEQPACKET") : constant("SOCK_STREAM");
        return (FileDescriptor) os("socket", new Class<?>[] { int.class, int.class, int.class }, constant("AF_UNIX"), type, 0);
    }

    private synchronized FileDescriptor socket() throws IOException {
        if (closed) throw new IOException("socket closed");
        if (fd == null) fd = newSocket(sockType);
        return fd;
    }

    // ---- what an app calls

    public void connect(LocalSocketAddress endpoint) throws IOException {
        synchronized (this) {
            if (connected) throw new IOException("already connected");
            os("connect", new Class<?>[] { FileDescriptor.class, java.net.SocketAddress.class }, socket(), unixAddress(endpoint));
            connected = bound = true;
            remoteAddress = endpoint;
        }
    }

    public void connect(LocalSocketAddress endpoint, int timeout) throws IOException { connect(endpoint); }

    public void bind(LocalSocketAddress bindpoint) throws IOException {
        synchronized (this) {
            if (bound) throw new IOException("already bound");
            os("bind", new Class<?>[] { FileDescriptor.class, java.net.SocketAddress.class }, socket(), unixAddress(bindpoint));
            bound = true;
            localAddress = bindpoint;
        }
    }

    public synchronized InputStream getInputStream() throws IOException {
        socket();
        if (in == null) in = new SocketInput();
        return in;
    }

    public synchronized OutputStream getOutputStream() throws IOException {
        socket();
        if (out == null) out = new SocketOutput();
        return out;
    }

    @Override
    public void close() throws IOException {
        FileDescriptor f;
        synchronized (this) {
            if (closed) return;
            closed = true;
            f = fd;
        }
        if (f != null && f.valid()) os("close", new Class<?>[] { FileDescriptor.class }, f);
    }

    public void shutdownInput() throws IOException {
        os("shutdown", new Class<?>[] { FileDescriptor.class, int.class }, socket(), constant("SHUT_RD"));
        inputShutdown = true;
    }

    public void shutdownOutput() throws IOException {
        os("shutdown", new Class<?>[] { FileDescriptor.class, int.class }, socket(), constant("SHUT_WR"));
        outputShutdown = true;
    }

    public void setSoTimeout(int n) throws IOException {
        soTimeout = n;
        try {
            Object tv = Class.forName("android.system.StructTimeval").getMethod("fromMillis", long.class).invoke(null, (long) n);
            os("setsockoptTimeval", new Class<?>[] { FileDescriptor.class, int.class, int.class, Class.forName("android.system.StructTimeval") },
                socket(), constant("SOL_SOCKET"), constant("SO_RCVTIMEO"), tv);
        } catch (ReflectiveOperationException e) {
            // (The time is kept; reads wait as long as they must.)
        }
    }

    public int getSoTimeout() { return soTimeout; }
    public void setReceiveBufferSize(int size) { receiveBufferSize = size; }
    public int getReceiveBufferSize() { return receiveBufferSize; }
    public void setSendBufferSize(int n) { sendBufferSize = n; }
    public int getSendBufferSize() { return sendBufferSize; }
    public LocalSocketAddress getRemoteSocketAddress() { return remoteAddress; }
    public LocalSocketAddress getLocalSocketAddress() { return localAddress; }
    public synchronized boolean isConnected() { return connected; }
    public synchronized boolean isClosed() { return closed; }
    public synchronized boolean isBound() { return bound; }
    public boolean isOutputShutdown() { return outputShutdown; }
    public boolean isInputShutdown() { return inputShutdown; }
    public void setFileDescriptorsForSend(FileDescriptor[] fds) { toSend = fds; }
    public FileDescriptor[] getAncillaryFileDescriptors() { return null; }
    /** The peer is of this app: its ids. */
    public Credentials getPeerCredentials() { return new Credentials(android.os.Process.myPid(), android.os.Process.myUid(), android.os.Process.myUid()); }
    public FileDescriptor getFileDescriptor() { return fd; }

    @Override
    public String toString() { return super.toString() + " impl:" + fd; }

    private final class SocketInput extends InputStream {
        @Override
        public int read() throws IOException {
            byte[] b = new byte[1];
            int n = read(b, 0, 1);
            return n <= 0 ? -1 : b[0] & 0xFF;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (len == 0) return 0;
            int n = (Integer) os("read", new Class<?>[] { FileDescriptor.class, byte[].class, int.class, int.class }, socket(), b, off, len);
            return n == 0 ? -1 : n;
        }

        @Override
        public void close() throws IOException { LocalSocket.this.close(); }
    }

    private final class SocketOutput extends OutputStream {
        @Override
        public void write(int b) throws IOException { write(new byte[] { (byte) b }, 0, 1); }

        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            while (len > 0) {
                int n = (Integer) os("write", new Class<?>[] { FileDescriptor.class, byte[].class, int.class, int.class }, socket(), b, off, len);
                off += n;
                len -= n;
            }
        }

        @Override
        public void close() throws IOException { LocalSocket.this.close(); }
    }
}
