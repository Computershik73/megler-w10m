package megler.test.plugin;

import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * The plug-in's service another app binds, as Total Commander binds its plug-ins' (megler.HostedApps runs it inside the app
 * that asks: the test app crossapp). Its Binder is written by hand as an AIDL compiler writes a Stub: the other app's proxy
 * is no class of this app's, so each call is a transaction with a Parcel. It tells where it runs -- its Context's package,
 * a string of its own resources, its files folder, its Application -- and calls back the Binder it is handed. And it binds
 * a service of its own that no other app may, and takes that one's Binder for the class it is: the plug-in's code is one
 * scope here, as in its own process (Total Commander's LAN plug-in does so with its ForegroundService).
 */
public class PluginService extends Service {
    private static final String T = "PluginTest";
    static final String DESCRIPTOR = "megler.test.plugin.IPlugin", CALLBACK = "megler.test.plugin.ICallback";
    /* describe(): the package, the label, the files folder, the Application's class, how many Applications and services were made, its own service bound. */
    static final int DESCRIBE = IBinder.FIRST_CALL_TRANSACTION;
    /* callBack(Binder, String): calls the Binder's first transaction with "plugin heard: <the string>", answers its answer plus one. */
    static final int CALL_BACK = IBinder.FIRST_CALL_TRANSACTION + 1;
    static int made;
    private volatile boolean own;

    private final ServiceConnection mine = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            own = binder instanceof Private.Local && ((Private.Local) binder).service().getApplication() == getApplication();
            Log.i(T, "its own service bound " + name.flattenToShortString() + " local=" + own);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {}
    };

    private final Binder binder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            switch (code) {
            case DESCRIBE:
                data.enforceInterface(DESCRIPTOR);
                reply.writeNoException();
                reply.writeString(getPackageName());
                reply.writeString(getString(R.string.app_name));
                reply.writeString(getFilesDir().getPath());
                reply.writeString(getApplication().getClass().getName());
                reply.writeInt(App.created);
                reply.writeInt(made);
                reply.writeInt(own ? 1 : 0);
                return true;
            case CALL_BACK: {
                data.enforceInterface(DESCRIPTOR);
                IBinder callback = data.readStrongBinder();
                String said = data.readString();
                Parcel d = Parcel.obtain(), r = Parcel.obtain();
                int answer;
                try {
                    d.writeInterfaceToken(CALLBACK);
                    d.writeString("plugin heard: " + said);
                    callback.transact(IBinder.FIRST_CALL_TRANSACTION, d, r, 0);
                    r.readException();
                    answer = r.readInt();
                } finally {
                    d.recycle();
                    r.recycle();
                }
                reply.writeNoException();
                reply.writeInt(answer + 1);
                return true;
            }
            default:
                return super.onTransact(code, data, reply, flags);
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        made++;
        Log.i(T, "service created " + made + " package=" + getPackageName());
        try (FileOutputStream out = new FileOutputStream(new File(getFilesDir(), "served.txt"))) {
            out.write(("served " + made).getBytes("UTF-8"));
        } catch (IOException e) {
            Log.e(T, "its file cannot be written", e);
        }
        boolean asked = bindService(new Intent(this, Private.class), mine, BIND_AUTO_CREATE);
        if (!asked) Log.e(T, "its own service was not bound");
    }

    @Override
    public IBinder onBind(Intent intent) {
        Log.i(T, "service bound action=" + intent.getAction());
        return binder;
    }

    @Override
    public void onDestroy() {
        unbindService(mine);
        Log.i(T, "service destroyed");
        super.onDestroy();
    }
}
