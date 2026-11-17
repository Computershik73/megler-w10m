package megler;

import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentValues;
import android.content.OperationApplicationException;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Who called a content provider of the app's. Everything runs in the app's one process here, Google Play services too
 * (hosted, in a class scope of their own), so the caller is the app -- its package (ContentProvider.getCallingPackage) and
 * its uid (Binder.getCallingUid) -- except for the length of a call that Google Play services' own code makes into a
 * provider of the app's: on Android that call comes from their process, and the provider is told their package and their
 * uid. Android's Binder keeps the caller for the thread while the call runs, and so does this (a Call, below).
 *
 * Why. Files by Google hands Quick Share a folder as a content:// URI of its FolderSharingContentProvider, whose query
 * answers no one but Google Play services (getCallingPackage is "com.google.android.gms", and the package is signed by
 * Google): told the app's own package, it threw UnsupportedOperationException("Operation not supported by
 * com.google.android.apps.nbu.files") on Google Play services' thread, and no folder was shared.
 *
 * The other way round is as it was: a provider of Google Play services' asked by the app is told the app (see
 * ContentProvider.getCallingPackage), and so is a provider of the app's asked by the app. Inside such a call the same
 * holds: another provider that the app's provider asks is told the app's package, and Google Play services' code that
 * it calls is told the app's uid -- on Android that is a call out of the app's process.
 */
public final class Callers {
    private Callers() {}

    /** A call of Google Play services' into a provider of the app's, running on a thread. */
    private static final class Call {
        final ContentProvider provider;
        final Call outer;                       /* the call this one is inside of, on the same thread, or null */
        boolean uidCleared, packageCleared;     /* Binder.clearCallingIdentity, ContentProvider.clearCallingIdentity */
        Call(ContentProvider provider, Call outer) { this.provider = provider; this.outer = outer; }
    }

    private static final ThreadLocal<Call> sCall = new ThreadLocal<>();
    /* How many such calls are running, on any thread: none, as nearly always, and nobody looks at the thread. */
    private static final AtomicInteger sRunning = new AtomicInteger();
    /* What clear() answers inside such a call, with what it cleared (UID, PACKAGE), which restore() then puts back. */
    private static final long TOKEN = 0x4D45474C43410000L, UID = 1, PACKAGE = 2;

    private static Call current() { return sRunning.get() == 0 ? null : sCall.get(); }

    /** What `self`.getCallingPackage() answers: Google Play services' package inside a call of theirs into `self`, else the app's. */
    public static String callingPackage(ContentProvider self) {
        Call c = current();
        return c != null && c.provider == self && !c.packageCleared ? GooglePackages.GMS : Native.packageName();
    }

    /**
     * What Binder.getCallingUid answers: Google Play services' uid on a thread inside a call of theirs into a provider of the
     * app's, asked by the app's code; else the app's uid -- and to Google Play services' own code inside such a call too,
     * which the app's provider has called then.
     */
    public static int callingUid() {
        Call c = current();
        return c != null && !c.uidCleared && !MicroG.calledByHosted() ? GooglePackages.sharedUid() : android.os.Process.myUid();
    }

    /**
     * Binder.clearCallingIdentity, and ContentProvider's (`packageToo`): the caller is the app from here on. The token says
     * what was cleared; 0 outside a call of Google Play services', where the caller is the app as it is.
     */
    public static long clear(boolean packageToo) {
        Call c = current();
        if (c == null) return 0;
        long token = TOKEN;
        if (!c.uidCleared) { c.uidCleared = true; token |= UID; }
        if (packageToo && !c.packageCleared) { c.packageCleared = true; token |= PACKAGE; }
        return token;
    }

    /** Binder.restoreCallingIdentity: what clear() took away, back. */
    public static void restore(long token) {
        Call c;
        if ((token & ~(UID | PACKAGE)) != TOKEN || (c = current()) == null) return;
        if ((token & UID) != 0) c.uidCleared = false;
        if ((token & PACKAGE) != 0) c.packageCleared = false;
    }

    /**
     * The provider that ContentResolver calls for `p`: `p` itself, or -- `p` a provider of the app's own, and the code asking
     * Google Play services' (hosted here) -- one that passes every call on to `p` with Google Play services as the caller.
     * Megler's own providers and Google Play services' are never wrapped.
     */
    public static ContentProvider through(ContentProvider p) {
        if (p == null || p instanceof Theirs) return p;
        ClassLoader own = p.getClass().getClassLoader();
        if (own == null || own == ContentProvider.class.getClassLoader() || MicroG.isGmsLoader(own)) return p;
        return MicroG.calledByHosted() ? new Theirs(p) : p;
    }

    private static Call enter(ContentProvider p) {
        Call c = new Call(p, sCall.get());
        sRunning.incrementAndGet();
        sCall.set(c);
        return c;
    }

    /* The thread is what it was before the call, whatever the provider cleared or restored meanwhile. */
    private static void leave(Call c) {
        if (c.outer == null) sCall.remove(); else sCall.set(c.outer);
        sRunning.decrementAndGet();
    }

    /** A provider of the app's as Google Play services' code is given it: each call runs with them as the caller. */
    private static final class Theirs extends ContentProvider {
        private final ContentProvider p;
        Theirs(ContentProvider p) { this.p = p; }
        @Override public boolean onCreate() { return true; }
        @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort) {
            Call c = enter(p);
            try { return p.query(u, proj, sel, args, sort); } finally { leave(c); }
        }
        @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort, CancellationSignal signal) {
            Call c = enter(p);
            try { return p.query(u, proj, sel, args, sort, signal); } finally { leave(c); }
        }
        @Override public Cursor query(Uri u, String[] proj, Bundle args, CancellationSignal signal) {
            Call c = enter(p);
            try { return p.query(u, proj, args, signal); } finally { leave(c); }
        }
        @Override public String getType(Uri u) {
            Call c = enter(p);
            try { return p.getType(u); } finally { leave(c); }
        }
        @Override public String[] getStreamTypes(Uri u, String filter) {
            Call c = enter(p);
            try { return p.getStreamTypes(u, filter); } finally { leave(c); }
        }
        @Override public Uri canonicalize(Uri u) {
            Call c = enter(p);
            try { return p.canonicalize(u); } finally { leave(c); }
        }
        @Override public Uri uncanonicalize(Uri u) {
            Call c = enter(p);
            try { return p.uncanonicalize(u); } finally { leave(c); }
        }
        @Override public boolean refresh(Uri u, Bundle extras, CancellationSignal signal) {
            Call c = enter(p);
            try { return p.refresh(u, extras, signal); } finally { leave(c); }
        }
        @Override public Uri insert(Uri u, ContentValues v) {
            Call c = enter(p);
            try { return p.insert(u, v); } finally { leave(c); }
        }
        @Override public Uri insert(Uri u, ContentValues v, Bundle extras) {
            Call c = enter(p);
            try { return p.insert(u, v, extras); } finally { leave(c); }
        }
        @Override public int bulkInsert(Uri u, ContentValues[] v) {
            Call c = enter(p);
            try { return p.bulkInsert(u, v); } finally { leave(c); }
        }
        @Override public int delete(Uri u, String sel, String[] args) {
            Call c = enter(p);
            try { return p.delete(u, sel, args); } finally { leave(c); }
        }
        @Override public int delete(Uri u, Bundle extras) {
            Call c = enter(p);
            try { return p.delete(u, extras); } finally { leave(c); }
        }
        @Override public int update(Uri u, ContentValues v, String sel, String[] args) {
            Call c = enter(p);
            try { return p.update(u, v, sel, args); } finally { leave(c); }
        }
        @Override public int update(Uri u, ContentValues v, Bundle extras) {
            Call c = enter(p);
            try { return p.update(u, v, extras); } finally { leave(c); }
        }
        @Override public Bundle call(String method, String arg, Bundle extras) {
            Call c = enter(p);
            try { return p.call(method, arg, extras); } finally { leave(c); }
        }
        @Override public Bundle call(String authority, String method, String arg, Bundle extras) {
            Call c = enter(p);
            try { return p.call(authority, method, arg, extras); } finally { leave(c); }
        }
        @Override public ContentProviderResult[] applyBatch(ArrayList<ContentProviderOperation> ops) throws OperationApplicationException {
            Call c = enter(p);
            try { return p.applyBatch(ops); } finally { leave(c); }
        }
        @Override public ContentProviderResult[] applyBatch(String authority, ArrayList<ContentProviderOperation> ops) throws OperationApplicationException {
            Call c = enter(p);
            try { return p.applyBatch(authority, ops); } finally { leave(c); }
        }
        @Override public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
            Call c = enter(p);
            try { return p.openFile(u, mode); } finally { leave(c); }
        }
        @Override public ParcelFileDescriptor openFile(Uri u, String mode, CancellationSignal signal) throws FileNotFoundException {
            Call c = enter(p);
            try { return p.openFile(u, mode, signal); } finally { leave(c); }
        }
        @Override public AssetFileDescriptor openAssetFile(Uri u, String mode) throws FileNotFoundException {
            Call c = enter(p);
            try { return p.openAssetFile(u, mode); } finally { leave(c); }
        }
        @Override public AssetFileDescriptor openAssetFile(Uri u, String mode, CancellationSignal signal) throws FileNotFoundException {
            Call c = enter(p);
            try { return p.openAssetFile(u, mode, signal); } finally { leave(c); }
        }
        @Override public AssetFileDescriptor openTypedAssetFile(Uri u, String mime, Bundle o) throws FileNotFoundException {
            Call c = enter(p);
            try { return p.openTypedAssetFile(u, mime, o); } finally { leave(c); }
        }
    }
}
