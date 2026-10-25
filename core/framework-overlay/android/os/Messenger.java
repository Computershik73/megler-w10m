// Overlay of core/framework/src/android/os/Messenger.java (tools/build-framework.sh).
// It differs in the binder a Messenger made for a Handler has: it says it is android.os.IMessenger, as the binder Android's
// Handler.getIMessenger() makes does, where the original's said nothing (null).
//
// Why. Firebase's MessengerIpcClient binds microG's GCM registration service (com.google.android.c2dm.intent.REGISTER),
// which answers with a Messenger's binder, and takes that binder only when its descriptor is android.os.IMessenger (or
// microG's IMessengerCompat): with null it said "Invalid interface descriptor: null", and every app's FCM token failed with
// SERVICE_NOT_AVAILABLE (Play Store: "Error when retrieving FCM instance id", package 385 on the phone).
//
// And a Messenger reaches a binder that is not a Messenger's of this process as Android's IMessenger proxy does, by a
// transaction, and a Messenger's binder answers one (Impl): Google Play services' task services hand their scheduler a
// binder of their own that passes transactions on to their Messenger's, and every send to it ended in a
// DeadObjectException -- no task of GmsCore's ran (its scheduler: "Unknown result code received: 4"), its first sync of its
// server flags (Phenotype) among them. A message received says its sender's uid (sendingUid), as Android's does; the
// scheduler and GmsCore's GCM check it. Everything else is the original's, unchanged.
package android.os;

/** A reference to a Handler other code can send messages to (in the same process here: straight to the handler). */
public final class Messenger implements Parcelable {
    private static final String DESCRIPTOR = "android.os.IMessenger";
    private final Handler mTarget;
    private final IBinder mBinder;
    /** The binder of a Messenger made for a Handler: an IMessenger, as Handler.getIMessenger() makes Android's. */
    private static final class Impl extends Binder {
        private final Handler target;
        Impl(Handler target) { super(DESCRIPTOR); this.target = target; }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            Message m = data.readInt() != 0 ? Message.CREATOR.createFromParcel(data) : null;
            if (m != null) { m.sendingUid = Binder.getCallingUid(); target.sendMessage(m); }
            return true;
        }
    }
    public Messenger(Handler target) { mTarget = target; mBinder = new Impl(target); }
    public Messenger(IBinder target) { mTarget = target instanceof Impl ? ((Impl) target).target : null; mBinder = target; }
    public void send(Message message) throws RemoteException {
        if (mTarget != null) { message.sendingUid = Binder.getCallingUid(); mTarget.sendMessage(message); return; }
        Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            if (message != null) { data.writeInt(1); message.writeToParcel(data, 0); } else data.writeInt(0);
            if (mBinder == null || !mBinder.transact(IBinder.FIRST_CALL_TRANSACTION, data, null, IBinder.FLAG_ONEWAY)) throw new DeadObjectException();
        } finally { data.recycle(); }
    }
    public IBinder getBinder() { return mBinder; }
    @Override public boolean equals(Object o) { return o instanceof Messenger && ((Messenger) o).mBinder == mBinder; }
    @Override public int hashCode() { return mBinder.hashCode(); }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel out, int flags) { out.writeStrongBinder(mBinder); }
    public static void writeMessengerOrNullToParcel(Messenger m, Parcel out) { out.writeStrongBinder(m != null ? m.mBinder : null); }
    public static Messenger readMessengerOrNullFromParcel(Parcel in) { IBinder b = in.readStrongBinder(); return b != null ? new Messenger(b) : null; }
    public static final Creator<Messenger> CREATOR = new Creator<Messenger>() { public Messenger createFromParcel(Parcel in) { IBinder target = in.readStrongBinder(); return target != null ? new Messenger(target) : null; } public Messenger[] newArray(int size) { return new Messenger[size]; } };
}
