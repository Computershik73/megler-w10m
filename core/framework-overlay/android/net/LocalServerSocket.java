// Overlay of core/framework/gen-src/android/net/LocalServerSocket.java (tools/build-framework.sh).
// It differs in listening, as Android's does: a socket of AF_UNIX bound to a name of the abstract namespace, whose accept
// gives a connected android.net.LocalSocket (an overlay too, which says how). The generated stub's accept gave null.
package android.net;

import java.io.FileDescriptor;
import java.io.IOException;

public class LocalServerSocket implements java.io.Closeable {
    private static final int LISTEN_BACKLOG = 50;

    private final FileDescriptor fd;
    private final LocalSocketAddress localAddress;
    private boolean closed;

    public LocalServerSocket(String name) throws IOException {
        localAddress = new LocalSocketAddress(name);
        fd = LocalSocket.newSocket(LocalSocket.SOCKET_STREAM);
        try {
            LocalSocket.os("bind", new Class<?>[] { FileDescriptor.class, java.net.SocketAddress.class }, fd, LocalSocket.unixAddress(localAddress));
            LocalSocket.os("listen", new Class<?>[] { FileDescriptor.class, int.class }, fd, LISTEN_BACKLOG);
        } catch (IOException e) {
            LocalSocket.os("close", new Class<?>[] { FileDescriptor.class }, fd);
            throw e;
        }
    }

    /** A socket made and bound already: it listens from now on. */
    public LocalServerSocket(FileDescriptor fd) throws IOException {
        this.fd = fd;
        localAddress = null;
        LocalSocket.os("listen", new Class<?>[] { FileDescriptor.class, int.class }, fd, LISTEN_BACKLOG);
    }

    public LocalSocketAddress getLocalSocketAddress() { return localAddress; }

    public LocalSocket accept() throws IOException {
        FileDescriptor peer = (FileDescriptor) LocalSocket.os("accept", new Class<?>[] { FileDescriptor.class, java.net.SocketAddress.class }, fd, null);
        return new LocalSocket(peer);
    }

    public FileDescriptor getFileDescriptor() { return fd; }

    @Override
    public void close() throws IOException {
        synchronized (this) {
            if (closed) return;
            closed = true;
        }
        LocalSocket.os("close", new Class<?>[] { FileDescriptor.class }, fd);
    }
}
