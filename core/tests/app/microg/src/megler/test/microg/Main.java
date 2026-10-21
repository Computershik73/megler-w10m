package megler.test.microg;

import android.app.Activity;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.util.Log;
import android.widget.TextView;

/** Integration test: transactions go to the official GmsCore APK, not a Megler binder. */
public class Main extends Activity {
    private static final String GMS = "com.google.android.gms";
    private static final String ACTION = "com.google.android.gms.ads.identifier.service.START";
    private static final String DESCRIPTOR = "com.google.android.gms.ads.identifier.internal.IAdvertisingIdService";
    private boolean done;
    private int connections;
    private IBinder first;
    private TextView text;
    private static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    private void fail(Throwable t) { done = true; Log.e("MicroGTest", "FAILED", t); text.setText("FAILED: " + t); }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this); text.setText("Checking microG..."); setContentView(text);
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(GMS, PackageManager.GET_SERVICES);
            check(pi.applicationInfo.sourceDir.endsWith("/GmsCore/GmsCore.apk"), "real APK metadata");
            check(pi.services != null && pi.services.length > 20, "manifest services");
            Context gms = createPackageContext(GMS, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
            check(GMS.equals(gms.getPackageName()), "package context");
            Class<?> client = com.google.android.gms.common.internal.GetServiceRequest.class;
            Class<?> service = Class.forName(client.getName(), false, gms.getClassLoader());
            check(service != client, "distinct definitions of GetServiceRequest");
            check(service.getClassLoader() == gms.getClassLoader(), "defining loader");
            check(service.getPackage() != null, "service package metadata");
            check(com.google.android.gms.org.conscrypt.NativeCrypto.CLIENT_MARKER == 731, "client native class loaded first");
            Class<?> provider = Class.forName("com.google.android.gms.org.conscrypt.OpenSSLProvider", true, gms.getClassLoader());
            check(provider.newInstance() instanceof java.security.Provider, "microG Conscrypt provider initializes");
            check(com.google.android.gms.org.conscrypt.NativeCrypto.CLIENT_MARKER == 731, "JNI registration preserves client class");
            check(service.getField("CREATOR").getType() != int.class, "service CREATOR type");
            check(client.getField("CREATOR").getInt(null) == 1729, "client CREATOR unchanged");
            check(java.lang.reflect.Array.newInstance(service, 1).getClass().getComponentType() == service, "array component loader");
            check(Class.forName("android.os.Binder", false, gms.getClassLoader()) == Binder.class, "shared platform");
            check(!gms.getFilesDir().equals(getFilesDir()), "separate files");
            check(!gms.getDatabasePath("x").equals(getDatabasePath("x")), "separate database");
            gms.getSharedPreferences("microg-probe", 0).edit().putString("owner", "microg").commit();
            check(!getSharedPreferences("microg-probe", 0).contains("owner"), "separate preferences");
            int label = gms.getResources().getIdentifier("gms_app_name", "string", GMS);
            if (label == 0) label = gms.getResources().getIdentifier("app_name", "string", GMS);
            check(label != 0, "package resources found");
            check(gms.getResources().getString(label).length() > 0, "package resource value");
            Intent i = new Intent(ACTION).setPackage(GMS);
            ResolveInfo r = getPackageManager().resolveService(i, 0);
            check(r != null && r.serviceInfo.name.equals("org.microg.gms.ads.identifier.AdvertisingIdService"), "real service resolution");
            check(getPackageManager().resolveService(new Intent("com.google.android.gms.NO_SUCH_SERVICE").setPackage(GMS), 0) == null, "unknown service is absent");
            check(!bindService(new Intent(ACTION).setPackage("wrong.package"), connection, BIND_AUTO_CREATE), "package routing");
            ServiceConnection cancelled = new ServiceConnection() {
                public void onServiceConnected(ComponentName n, IBinder b) { fail(new AssertionError("callback after immediate unbind")); }
                public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("cancelled binding disconnected")); }
                public void onNullBinding(ComponentName n) { fail(new AssertionError("cancelled null binding")); }
            };
            check(bindService(i, cancelled, BIND_AUTO_CREATE), "pending bind accepted");
            unbindService(cancelled);
            check(bindService(i, connection, BIND_AUTO_CREATE), "bind accepted");
            new Handler().postDelayed(() -> { if (!done) fail(new AssertionError("binding timeout")); }, 30000);
        } catch (Throwable t) { fail(t); }
    }
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder b) {
            try {
                check(GMS.equals(name.getPackageName()), "callback package");
                check(DESCRIPTOR.equals(b.getInterfaceDescriptor()), "binder descriptor");
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    check(b.transact(1, data, reply, 0), "get ID transaction"); reply.readException();
                    check("00000000-0000-0000-0000-000000000000".equals(reply.readString()), "microG default limited identifier");
                } finally { data.recycle(); reply.recycle(); }
                data = Parcel.obtain(); reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR); data.writeInt(1);
                    check(b.transact(2, data, reply, 0), "tracking limit transaction"); reply.readException();
                    check(reply.readInt() != 0, "tracking limited by default");
                } finally { data.recycle(); reply.recycle(); }
                if (++connections == 1) {
                    first = b; unbindService(this);
                    new Handler().post(() -> {
                        try { check(bindService(new Intent(ACTION).setPackage(GMS), connection, BIND_AUTO_CREATE), "rebind accepted"); }
                        catch (Throwable t) { fail(t); }
                    });
                } else {
                    check(first != b, "service destroyed after last unbind");
                    unbindService(this);
                    check(bindService(new Intent("com.google.android.gms.common.service.START").setPackage(GMS), brokerConnection, BIND_AUTO_CREATE), "broker bind");
                }
            } catch (Throwable t) { fail(t); }
        }
        @Override public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("disconnected: " + n)); }
        @Override public void onNullBinding(ComponentName n) { fail(new AssertionError("null binding: " + n)); }
    };
    private final ServiceConnection brokerConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                check("com.google.android.gms.common.internal.IGmsServiceBroker".equals(binder.getInterfaceDescriptor()), "broker descriptor");
                final boolean[] called = {false};
                Binder callback = new Binder() {
                    @Override protected boolean onTransact(int code, Parcel in, Parcel out, int flags) throws RemoteException {
                        if (code != 1) return super.onTransact(code, in, out, flags);
                        in.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                        check(in.readInt() == 0, "broker success status");
                        IBinder service = in.readStrongBinder();
                        check(service != null && "com.google.android.gms.common.internal.service.ICommonService".equals(service.getInterfaceDescriptor()), "broker service binder");
                        called[0] = true; out.writeNoException(); return true;
                    }
                };
                data.writeInterfaceToken("com.google.android.gms.common.internal.IGmsServiceBroker");
                data.writeStrongBinder(callback); data.writeInt(1);
                // SafeParcelable GetServiceRequest, encoded on the client side without loading microG classes.
                data.writeInt(0xffff4f45); int sizeAt = data.dataPosition(); data.writeInt(0); int start = data.dataPosition();
                data.writeInt(0x40001); data.writeInt(6);
                data.writeInt(0x40002); data.writeInt(39);
                data.writeInt(0x40003); data.writeInt(252432034);
                data.writeInt(0xffff0004); int fieldAt = data.dataPosition(); data.writeInt(0); int fieldStart = data.dataPosition();
                data.writeString(getPackageName()); int fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                data.writeInt(0xffff0007); fieldAt = data.dataPosition(); data.writeInt(0); fieldStart = data.dataPosition();
                data.writeBundle(new Bundle()); fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                data.setDataPosition(sizeAt); data.writeInt(fieldEnd - start); data.setDataPosition(fieldEnd);
                check(binder.transact(46, data, reply, 0), "getService transaction"); reply.readException();
                check(called[0], "broker callback delivered");
                check(com.google.android.gms.common.internal.GetServiceRequest.CREATOR == 1729, "client survives broker call");
                unbindService(this); done = true;
                Log.i("MicroGTest", "PASS isolated GmsCore broker, duplicate classes, reflection, arrays, advertising ID, storage and resources");
                text.setText("microG isolated services: PASS");
            } catch (Throwable t) { fail(t); }
            finally { data.recycle(); reply.recycle(); }
        }
        public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("broker disconnected")); }
        public void onNullBinding(ComponentName n) { fail(new AssertionError("broker null binding")); }
    };

}
