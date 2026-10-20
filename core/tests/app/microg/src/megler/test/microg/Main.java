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
                    unbindService(this); done = true;
                    Log.i("MicroGTest", "PASS official GmsCore bind, transactions, resources, storage, unbind/rebind");
                    text.setText("microG services: PASS");
                }
            } catch (Throwable t) { fail(t); }
        }
        @Override public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("disconnected: " + n)); }
        @Override public void onNullBinding(ComponentName n) { fail(new AssertionError("null binding: " + n)); }
    };
}
