// Overlay of core/framework/src/android/app/PendingIntent.java (tools/build-framework.sh).
// It differs in getIntentSender, which gave an IntentSender that sent nothing: here one that sends this PendingIntent
// (android.content.IntentSender.meglerOf); in its creator, the package of the context that made it, not always the
// app's; and in that one is found again by what it starts, with its flags and cancel (get). And in a Parcel: written as
// itself, in the parcel's table of this process's objects (writeToParcel, CREATOR, write/readPendingIntentOrNull...), where
// the original wrote nothing and read null -- Google Play services' answers carry PendingIntents in their SafeParcelables,
// and Play Store's install (its billing sheet's) got null for one and ended (package 539). The rest is the original's,
// unchanged.
package android.app;

import android.content.Context;
import android.content.Intent;

/** A pending intent, held in this process; sending it starts it as the app would. */
public final class PendingIntent implements android.os.Parcelable {
    public static final int FLAG_ONE_SHOT = 0x40000000, FLAG_NO_CREATE = 0x20000000, FLAG_CANCEL_CURRENT = 0x10000000, FLAG_UPDATE_CURRENT = 0x08000000,
        FLAG_IMMUTABLE = 0x04000000, FLAG_MUTABLE = 0x02000000, FLAG_ALLOW_UNSAFE_IMPLICIT_INTENT = 0x01000000;
    public static class CanceledException extends android.util.AndroidException { public CanceledException() {} public CanceledException(String s) { super(s); } public CanceledException(Exception e) { super(e); } }
    public interface OnFinished { void onSendFinished(PendingIntent p, Intent i, int resultCode, String resultData, android.os.Bundle resultExtras); }
    private final Context mContext; private final Intent mIntent; private final int mKind;   /* 0 activity, 1 broadcast, 2 service */
    private final int mRequest, mFlags; private final String mCreator;
    private volatile boolean mCanceled;
    private PendingIntent(Context c, Intent i, int kind, int req, int flags, String creator) {
        mContext = c; mIntent = new Intent(i); mKind = kind; mRequest = req; mFlags = flags; mCreator = creator;
    }
    /*
     * The PendingIntents of this process, found again as Android's activity manager finds its: by what they start (the kind,
     * the creator's package, the request code, and the intent but for its extras: Intent.filterEquals), while something holds
     * them, an alarm or the app. One asked for again is the same one, with its first extras, or with the new ones for
     * FLAG_UPDATE_CURRENT; FLAG_CANCEL_CURRENT makes a new one in its stead, and FLAG_NO_CREATE gives none when there is none.
     * GmsCore asks with FLAG_NO_CREATE whether its watchdog of module setup is set, and goes on with the setup only when it is
     * not: with a new PendingIntent for every answer, its setup never went on.
     */
    private static final java.util.HashMap<Integer, java.util.ArrayList<java.lang.ref.WeakReference<PendingIntent>>> sLive = new java.util.HashMap<>();
    private static PendingIntent get(Context c, int req, Intent i, int flags, int kind) {
        String creator = c != null && c.getPackageName() != null ? c.getPackageName() : megler.Native.packageName();
        Integer key = java.util.Objects.hash(kind, req, creator, i.filterHashCode());
        synchronized (sLive) {
            java.util.ArrayList<java.lang.ref.WeakReference<PendingIntent>> same = sLive.get(key);
            if (same == null) sLive.put(key, same = new java.util.ArrayList<>());
            PendingIntent had = null;
            for (java.util.Iterator<java.lang.ref.WeakReference<PendingIntent>> it = same.iterator(); it.hasNext(); ) {
                PendingIntent p = it.next().get();
                if (p == null || p.mCanceled) { it.remove(); continue; }
                if (p.mKind == kind && p.mRequest == req && p.mCreator.equals(creator) && p.mIntent.filterEquals(i)) had = p;
            }
            if (had != null && (flags & FLAG_CANCEL_CURRENT) != 0) { had.cancel(); had = null; }
            if (had != null) {
                if ((flags & FLAG_UPDATE_CURRENT) != 0) had.mIntent.replaceExtras(i);
                return had;
            }
            if ((flags & FLAG_NO_CREATE) != 0) { if (same.isEmpty()) sLive.remove(key); return null; }
            PendingIntent p = new PendingIntent(c, i, kind, req, flags, creator);
            same.add(new java.lang.ref.WeakReference<>(p));
            return p;
        }
    }
    public static PendingIntent getActivity(Context c, int req, Intent i, int flags) { return get(c, req, i, flags, 0); }
    public static PendingIntent getActivity(Context c, int req, Intent i, int flags, android.os.Bundle o) { return get(c, req, i, flags, 0); }
    public static PendingIntent getActivities(Context c, int req, Intent[] i, int flags) { return get(c, req, i[i.length - 1], flags, 0); }
    public static PendingIntent getBroadcast(Context c, int req, Intent i, int flags) { return get(c, req, i, flags, 1); }
    public static PendingIntent getService(Context c, int req, Intent i, int flags) { return get(c, req, i, flags, 2); }
    public static PendingIntent getForegroundService(Context c, int req, Intent i, int flags) { return get(c, req, i, flags, 2); }
    public void send() throws CanceledException { send(mContext, 0, null); }
    public void send(int code) throws CanceledException { send(mContext, code, null); }
    public void send(Context c, int code, Intent fill) throws CanceledException {
        if (mCanceled) throw new CanceledException();
        Intent i = new Intent(mIntent);
        if (fill != null) i.putExtras(fill);
        if ((mFlags & FLAG_ONE_SHOT) != 0) cancel();
        if (mKind == 0) mContext.startActivity(i); else if (mKind == 1) mContext.sendBroadcast(i); else mContext.startService(i);
    }
    public void send(int code, OnFinished f, android.os.Handler h) throws CanceledException { send(); }
    public void send(Context c, int code, Intent i, OnFinished f, android.os.Handler h) throws CanceledException { send(c, code, i); }
    /** Ends it: sending it fails, an alarm with it goes (AlarmManager.meglerCanceled), and asking for it again makes a new one. */
    public void cancel() { mCanceled = true; AlarmManager.meglerCanceled(this); }
    /** The intent it starts or sends (Activity.startIntentSenderForResult). */
    public Intent meglerIntent() { return mIntent; }
    /* The package of the context that made it, as Android records it: GmsCore's code in a client's process makes its own with
       its own context, and its task scheduler takes the package of a task's service from this (GcmNetworkManager's "app"). */
    public String getCreatorPackage() {
        String p = mContext == null ? null : mContext.getPackageName();
        return p != null ? p : megler.Native.packageName();
    }
    public String getTargetPackage() { return getCreatorPackage(); }
    public int getCreatorUid() {
        String p = getCreatorPackage();
        if (!p.equals(megler.Native.packageName())) try { return mContext.getPackageManager().getPackageUid(p, 0); } catch (Exception e) {}
        return android.os.Process.myUid();
    }
    public android.os.UserHandle getCreatorUserHandle() { return android.os.UserHandle.getUserHandleForUid(0); }
    public android.content.IntentSender getIntentSender() { return android.content.IntentSender.meglerOf(this); }
    public boolean isImmutable() { return true; } public boolean isActivity() { return mKind == 0; } public boolean isBroadcast() { return mKind == 1; } public boolean isService() { return mKind == 2; }
    public int describeContents() { return 0; }
    /* Itself, in the parcel's table of this process's objects (Parcel.writeParcelable): it is one of this process's. */
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeParcelable(this, flags); }
    public static final android.os.Parcelable.Creator<PendingIntent> CREATOR = new android.os.Parcelable.Creator<PendingIntent>() {
        @Override public PendingIntent createFromParcel(android.os.Parcel in) { return in.readParcelable(PendingIntent.class.getClassLoader()); }
        @Override public PendingIntent[] newArray(int n) { return new PendingIntent[n]; }
    };
    public static void writePendingIntentOrNullToParcel(PendingIntent p, android.os.Parcel out) { out.writeParcelable(p, 0); }
    public static PendingIntent readPendingIntentOrNullFromParcel(android.os.Parcel in) { return in.readParcelable(PendingIntent.class.getClassLoader()); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final long BLOCK_MUTABLE_IMPLICIT_PENDING_INTENT = 236704164L;
    public static final int FLAG_MUTABLE_UNAUDITED = 33554432;
    public static final long PENDING_INTENT_OPTIONS_CHECK = 320664730L;
    public static android.app.PendingIntent getActivities(android.content.Context p0, int p1, android.content.Intent[] p2, int p3, android.os.Bundle p4) { return null; }
    public static android.app.PendingIntent getActivitiesAsUser(android.content.Context p0, int p1, android.content.Intent[] p2, int p3, android.os.Bundle p4, android.os.UserHandle p5) { return null; }
    public static android.app.PendingIntent getActivityAsUser(android.content.Context p0, int p1, android.content.Intent p2, int p3, android.os.Bundle p4, android.os.UserHandle p5) { return null; }
    public static android.app.PendingIntent getBroadcastAsUser(android.content.Context p0, int p1, android.content.Intent p2, int p3, android.os.UserHandle p4) { return null; }
    public static boolean isNewMutableDisallowedImplicitPendingIntent(int p0, android.content.Intent p1, boolean p2) { return false; }
    public static void setOnMarshaledListener(android.app.PendingIntent.OnMarshaledListener p0) {}
    public boolean addCancelListener(java.util.concurrent.Executor p0, android.app.PendingIntent.CancelListener p1) { return false; }
    public void dumpDebug(android.util.proto.ProtoOutputStream p0, long p1) {}
    public android.content.Intent getIntent() { return null; }
    public java.lang.String getTag(java.lang.String p0) { return null; }
    public android.os.IBinder getWhitelistToken() { return null; }
    public boolean intentFilterEquals(android.app.PendingIntent p0) { return false; }
    public boolean isForegroundService() { return false; }
    public boolean isTargetedToPackage() { return false; }
    public java.util.List queryIntentComponents(int p0) { return new java.util.ArrayList(); }
    public void registerCancelListener(android.app.PendingIntent.CancelListener p0) {}
    public void removeCancelListener(android.app.PendingIntent.CancelListener p0) {}
    public void send(android.content.Context p0, int p1, android.content.Intent p2, android.app.PendingIntent.OnFinished p3, android.os.Handler p4, java.lang.String p5) {}
    public void send(android.content.Context p0, int p1, android.content.Intent p2, android.app.PendingIntent.OnFinished p3, android.os.Handler p4, java.lang.String p5, android.os.Bundle p6) {}
    public void send(android.os.Bundle p0) {}
    public int sendAndReturnResult(android.content.Context p0, int p1, android.content.Intent p2, android.app.PendingIntent.OnFinished p3, android.os.Handler p4, java.lang.String p5, android.os.Bundle p6) { return 0; }
    public void unregisterCancelListener(android.app.PendingIntent.CancelListener p0) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public interface CancelListener {
        void onCanceled(android.app.PendingIntent p0);
    }
    public interface OnMarshaledListener {
        void onMarshaled(android.app.PendingIntent p0, android.os.Parcel p1, int p2);
    }
    // ---- end of generated nested classes
}
