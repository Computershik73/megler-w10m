// Overlay of core/framework/src/android/os/ParcelFileDescriptor.java (tools/build-framework.sh).
// It differs in what dup and the descriptors it is given own, which is now Android's:
//  - dup(FileDescriptor), dup() and fromFd(int) make a descriptor of their own (Os.dup), and the result owns it: close()
//    closes that copy, and the original stays as it was. The original's wrapped the same descriptor and owned nothing.
//  - adoptFd(int) takes the descriptor over: close() closes it.
//  - detachFd() gives the number away: close() no longer closes it.
//
// Why. Code duplicates a descriptor and then closes its own: GeckoView's GeckoRuntime.startCrashHelper (Firefox 152)
// makes two socket pairs with Os.socketpair, wraps each end with ParcelFileDescriptor.dup, closes the four originals,
// and hands the duplicates' numbers to Gecko with detachFd. With no duplicate those numbers were the closed ones' (-1, as
// libcore leaves a closed FileDescriptor), and Gecko's crash helper client stopped on "fd != -1" (a MOZ_CRASH) as Gecko
// started.
//
// And it goes into a Parcel and comes out of one as Android's does (writeToParcel, CREATOR): a duplicate of its descriptor,
// which the one read from the Parcel owns, so that the writer may close its own. It had neither, and the CREATOR was null:
// microG's People service hands Play Store the account's picture so (IPeopleCallbacks.onParcelFileDescriptor), and Play
// Store's stub stopped on a NullPointerException as it read it (no avatar).
//
// And the file it was opened from is said (meglerFile), for android.content.res.loader.ResourcesProvider.loadFromApk, which
// opens an APK by its descriptor: Google Play services' modules' resources.
//
// Everything else is the original's, unchanged.
package android.os;

import java.io.*;

public class ParcelFileDescriptor implements Parcelable, Closeable {
    public static final int MODE_READ_ONLY = 0x10000000, MODE_WRITE_ONLY = 0x20000000, MODE_READ_WRITE = 0x30000000, MODE_CREATE = 0x08000000,
        MODE_TRUNCATE = 0x04000000, MODE_APPEND = 0x02000000, MODE_WORLD_READABLE = 1, MODE_WORLD_WRITEABLE = 2;
    private final FileDescriptor mFd;
    private final Closeable mOwner;
    private final File mFile;
    ParcelFileDescriptor(FileDescriptor fd, Closeable owner, File f) { mFd = fd; mOwner = owner; mFile = f; }
    public ParcelFileDescriptor(ParcelFileDescriptor o) { this(o.mFd, o.mOwner, o.mFile); }
    public static int parseMode(String m) {
        int r;
        if ("r".equals(m)) r = MODE_READ_ONLY;
        else if ("w".equals(m) || "wt".equals(m)) r = MODE_WRITE_ONLY | MODE_CREATE | MODE_TRUNCATE;
        else if ("wa".equals(m)) r = MODE_WRITE_ONLY | MODE_CREATE | MODE_APPEND;
        else if ("rw".equals(m)) r = MODE_READ_WRITE | MODE_CREATE;
        else if ("rwt".equals(m)) r = MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE;
        else throw new IllegalArgumentException("Bad mode '" + m + "'");
        return r;
    }
    public static ParcelFileDescriptor open(File f, int mode) throws FileNotFoundException {
        try {
            if ((mode & MODE_READ_WRITE) == MODE_READ_ONLY) { FileInputStream in = new FileInputStream(f); return new ParcelFileDescriptor(in.getFD(), in, f); }
            if ((mode & MODE_READ_WRITE) == MODE_WRITE_ONLY) { FileOutputStream out = new FileOutputStream(f, (mode & MODE_APPEND) != 0); return new ParcelFileDescriptor(out.getFD(), out, f); }
            RandomAccessFile raf = new RandomAccessFile(f, "rw");
            if ((mode & MODE_TRUNCATE) != 0) raf.setLength(0);
            return new ParcelFileDescriptor(raf.getFD(), raf, f);
        } catch (FileNotFoundException e) { throw e; } catch (IOException e) { throw new FileNotFoundException(e.toString()); }
    }
    /** Megler: a FileDescriptor object for a descriptor number. */
    private static FileDescriptor meglerOf(int fd) { FileDescriptor f = new FileDescriptor(); try { java.lang.reflect.Field d = FileDescriptor.class.getDeclaredField("descriptor"); d.setAccessible(true); d.setInt(f, fd); } catch (Exception e) {} return f; }
    /** Megler: libcore's android.system.Os.dup or close, by reflection (the stubs the framework is compiled against have no Os). */
    private static Object meglerOs(String name, FileDescriptor fd) throws IOException {
        try {
            return Class.forName("android.system.Os").getMethod(name, FileDescriptor.class).invoke(null, fd);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new IOException(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IOException(e);
        }
    }
    /** Megler: what closes a descriptor this object owns, once. */
    private static Closeable meglerCloser(final FileDescriptor fd) {
        return () -> { if (fd.valid()) meglerOs("close", fd); };
    }
    /** Megler: a descriptor of its own for `fd` (Os.dup), which closing the result closes, as Android's dup makes. */
    private static ParcelFileDescriptor meglerDup(FileDescriptor fd, File f) throws IOException {
        FileDescriptor copy = (FileDescriptor) meglerOs("dup", fd);
        return new ParcelFileDescriptor(copy, meglerCloser(copy), f);
    }
    public static ParcelFileDescriptor dup(FileDescriptor fd) throws IOException { return meglerDup(fd, null); }
    public static ParcelFileDescriptor fromFd(int fd) throws IOException { return meglerDup(meglerOf(fd), null); }
    public static ParcelFileDescriptor adoptFd(int fd) { FileDescriptor f = meglerOf(fd); return new ParcelFileDescriptor(f, meglerCloser(f), null); }
    public static ParcelFileDescriptor[] createPipe() throws IOException { throw new IOException("pipes are not supported"); }
    public static ParcelFileDescriptor[] createReliablePipe() throws IOException { return createPipe(); }
    public ParcelFileDescriptor dup() throws IOException { return meglerDup(mFd, mFile); }
    public FileDescriptor getFileDescriptor() { return mFd; }
    public int getFd() { try { java.lang.reflect.Field d = FileDescriptor.class.getDeclaredField("descriptor"); d.setAccessible(true); return d.getInt(mFd); } catch (Exception e) { return -1; } }
    /* Detached: the number is the caller's, and closing this no longer closes it. */
    private boolean mDetached;
    public int detachFd() { mDetached = true; return getFd(); }
    public long getStatSize() { return mFile != null ? mFile.length() : -1; }
    /** Megler: the file this was opened from (open), or null: what ResourcesProvider.loadFromApk opens the APK by. */
    public File meglerFile() { return mFile; }
    public void close() throws IOException { if (mOwner != null && !mDetached) mOwner.close(); }
    public void closeWithError(String msg) throws IOException { close(); }
    public void checkError() throws IOException {}
    public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
    /** As Android's: no channel for errors (0), then the descriptor -- here a duplicate, which the one read owns. */
    @Override
    public void writeToParcel(Parcel out, int flags) {
        out.writeInt(0);
        FileDescriptor copy;
        try { copy = (FileDescriptor) meglerOs("dup", mFd); } catch (IOException e) { copy = null; }
        out.writeRawFileDescriptor(copy);
        if ((flags & PARCELABLE_WRITE_RETURN_VALUE) != 0) { try { close(); } catch (IOException e) {} }
    }
    public static final Parcelable.Creator<ParcelFileDescriptor> CREATOR = new Parcelable.Creator<ParcelFileDescriptor>() {
        @Override
        public ParcelFileDescriptor createFromParcel(Parcel in) {
            if (in.readInt() != 0) in.readRawFileDescriptor();
            FileDescriptor fd = in.readRawFileDescriptor();
            return fd == null ? null : new ParcelFileDescriptor(fd, meglerCloser(fd), null);
        }
        @Override
        public ParcelFileDescriptor[] newArray(int size) { return new ParcelFileDescriptor[size]; }
    };
    public static class AutoCloseInputStream extends FileInputStream {
        private final ParcelFileDescriptor mPfd;
        public AutoCloseInputStream(ParcelFileDescriptor p) { super(p.getFileDescriptor()); mPfd = p; }
        @Override public void close() throws IOException { try { super.close(); } finally { mPfd.close(); } }
    }
    public static class AutoCloseOutputStream extends FileOutputStream {
        private final ParcelFileDescriptor mPfd;
        public AutoCloseOutputStream(ParcelFileDescriptor p) { super(p.getFileDescriptor()); mPfd = p; }
        @Override public void close() throws IOException { try { super.close(); } finally { mPfd.close(); } }
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static android.os.ParcelFileDescriptor[] createReliableSocketPair() { return null; }
    public static android.os.ParcelFileDescriptor[] createReliableSocketPair(int p0) { return null; }
    public static android.os.ParcelFileDescriptor[] createSocketPair() { return null; }
    public static android.os.ParcelFileDescriptor[] createSocketPair(int p0) { return null; }
    public static android.os.ParcelFileDescriptor fromData(byte[] p0, java.lang.String p1) { return null; }
    public static android.os.ParcelFileDescriptor fromDatagramSocket(java.net.DatagramSocket p0) { return null; }
    public static android.os.ParcelFileDescriptor fromFd(java.io.FileDescriptor p0, android.os.Handler p1, android.os.ParcelFileDescriptor.OnCloseListener p2) { return null; }
    public static android.os.ParcelFileDescriptor fromSocket(java.net.Socket p0) { return null; }
    public static java.io.File getFile(java.io.FileDescriptor p0) { return null; }
    public static android.os.ParcelFileDescriptor open(java.io.File p0, int p1, android.os.Handler p2, android.os.ParcelFileDescriptor.OnCloseListener p3) { return null; }
    public static android.os.ParcelFileDescriptor wrap(android.os.ParcelFileDescriptor p0, android.os.Handler p1, android.os.ParcelFileDescriptor.OnCloseListener p2) { return null; }
    public boolean canDetectErrors() { return false; }
    public void releaseResources() {}
    public long seekTo(long p0) { return 0L; }
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static class FileDescriptorDetachedException extends java.io.IOException {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public FileDescriptorDetachedException() { super(); }
    }
    public interface OnCloseListener {
        void onClose(java.io.IOException p0);
    }
    // ---- end of generated nested classes
}
