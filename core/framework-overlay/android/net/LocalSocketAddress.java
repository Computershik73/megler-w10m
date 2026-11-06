// Overlay of core/framework/gen-src/android/net/LocalSocketAddress.java (tools/build-framework.sh).
// It differs in keeping its name and namespace, as Android's does: the generated stub kept neither, and android.net.LocalSocket
// (an overlay too) connects and binds by them.
package android.net;

public class LocalSocketAddress {
    public enum Namespace {
        /** A name of Linux's abstract namespace, which no file stands for. */
        ABSTRACT,
        /** A name in Android's reserved namespace: a socket in /dev/socket. */
        RESERVED,
        /** A path in the file system. */
        FILESYSTEM
    }

    private final String name;
    private final Namespace namespace;

    public LocalSocketAddress(String name, Namespace namespace) {
        this.name = name;
        this.namespace = namespace;
    }

    public LocalSocketAddress(String name) { this(name, Namespace.ABSTRACT); }

    public String getName() { return name; }
    public Namespace getNamespace() { return namespace; }
}
