package megler.test.droidguard;

import android.app.Activity;
import android.content.*;
import android.os.*;
import android.util.Log;
import android.widget.TextView;
import java.io.*;

/** Opt-in network diagnostic for Google's GmsCore 26.43.30, hosted in this app's process. */
public class Main extends Activity {
    public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this); text.setText("DroidGuard capture"); setContentView(text);
        try {
            int version = getPackageManager().getPackageInfo("com.google.android.gms", 0).versionCode;
            if (version != 264330027) throw new IllegalStateException("This probe needs GmsCore 264330027; found " + version);
            final Context gms = createPackageContext("com.google.android.gms", CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
            File module = new File(getCodeCacheDir(), "capture.dex");
            try (InputStream in = getAssets().open("module.dex"); FileOutputStream out = new FileOutputStream(module)) {
                byte[] buffer = new byte[8192]; int n;
                while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            }
            // The hook bridge belongs to this child, not the parent class path GMS's VM searches.
            ClassLoader child = new dalvik.system.DexClassLoader(module.getPath(), getCodeCacheDir().getPath(), null, getClassLoader());
            child.loadClass("capture.Probe").getMethod("install", ClassLoader.class, File.class)
                .invoke(null, gms.getClassLoader(), new File(getFilesDir(), "capture"));
            bindService(new Intent("com.google.android.gms.droidguard.service.START").setPackage("com.google.android.gms"), new ServiceConnection() {
                public void onServiceDisconnected(ComponentName name) {}
                public void onServiceConnected(ComponentName name, IBinder binder) {
                    Log.i("DGCapture", "DroidGuard bound");
                    new Thread(new Runnable() { public void run() {
                        try {
                            Thread.sleep(45000);
                            Class<?> cls = gms.getClassLoader().loadClass("arcf");
                            Object manager = cls.getMethod("a", Context.class).invoke(null, gms);
                            Log.i("DGCapture", "Device key obtained=" + cls.getMethod("b").invoke(manager));
                        } catch (Throwable error) { Log.e("DGCapture", "device key request", error); }
                    }}, "capture-devicekey").start();
                }
            }, BIND_AUTO_CREATE);
        } catch (Throwable error) { Log.e("DGCapture", "installation failed", error); }
    }
}
