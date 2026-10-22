// Overlay of core/framework/src/android/os/Messenger.java (tools/build-framework.sh).
// It differs in the binder a Messenger made for a Handler has: it says it is android.os.IMessenger, as the binder Android's
// Handler.getIMessenger() makes does, where the original's said nothing (null).
//
// Why. Firebase's MessengerIpcClient binds microG's GCM registration service (com.google.android.c2dm.intent.REGISTER),
// which answers with a Messenger's binder, and takes that binder only when its descriptor is android.os.IMessenger (or
// microG's IMessengerCompat): with null it said "Invalid interface descriptor: null", and every app's FCM token failed with
// SERVICE_NOT_AVAILABLE (Play Store: "Error when retrieving FCM instance id", package 385 on the phone).
// Everything else is the original's, unchanged.
package android.os;

/** A reference to a Handler other code can send messages to (in the same process here: straight to the handler). */
public final class Messenger implements Parcelable {
    private static final String DESCRIPTOR = "android.os.IMessenger";
    private final Handler mTarget;
    private final IBinder mBinder;
    public Messenger(Handler target) { mTarget = target; mBinder = new Binder(DESCRIPTOR); MESSENGERS.put(mBinder, this); }
    public Messenger(IBinder target) { Messenger m = MESSENGERS.get(target); mTarget = m != null ? m.mTarget : null; mBinder = target; }
    private static final java.util.WeakHashMap<IBinder, Messenger> MESSENGERS = new java.util.WeakHashMap<>();
    public void send(Message message) throws RemoteException { if (mTarget == null) throw new DeadObjectException(); mTarget.sendMessage(message); }
    public IBinder getBinder() { return mBinder; }
    @Override public boolean equals(Object o) { return o instanceof Messenger && ((Messenger) o).mBinder == mBinder; }
    @Override public int hashCode() { return mBinder.hashCode(); }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel out, int flags) { out.writeStrongBinder(mBinder); }
    public static void writeMessengerOrNullToParcel(Messenger m, Parcel out) { out.writeStrongBinder(m != null ? m.mBinder : null); }
    public static Messenger readMessengerOrNullFromParcel(Parcel in) { IBinder b = in.readStrongBinder(); return b != null ? new Messenger(b) : null; }
    public static final Creator<Messenger> CREATOR = new Creator<Messenger>() { public Messenger createFromParcel(Parcel in) { IBinder target = in.readStrongBinder(); return target != null ? new Messenger(target) : null; } public Messenger[] newArray(int size) { return new Messenger[size]; } };
}
