// Overlay of core/framework/gen-src/android/net/Credentials.java (tools/build-framework.sh).
// It differs in keeping the ids it is made with, as Android's does (LocalSocket.getPeerCredentials).
package android.net;

public class Credentials {
    private final int pid;
    private final int uid;
    private final int gid;

    public Credentials(int pid, int uid, int gid) {
        this.pid = pid;
        this.uid = uid;
        this.gid = gid;
    }

    public int getPid() { return pid; }
    public int getUid() { return uid; }
    public int getGid() { return gid; }
}
