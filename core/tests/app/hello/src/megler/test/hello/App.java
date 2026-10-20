package megler.test.hello;

import android.app.Application;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;

/** The Application: says that it was made and created, in the order the test expects, and what its class loader is. */
public class App extends Application {
    static int created;

    @Override
    public void onCreate() {
        super.onCreate();
        created++;
        Log.i("HelloTest", "App.onCreate package=" + getPackageName());
        // What Android gives every app: one loader for all its classes, which also finds what else is in the APK.
        ClassLoader loader = getClassLoader();
        boolean same = loader == App.class.getClassLoader() && loader == MainActivity.class.getClassLoader();
        boolean manifest;
        try (InputStream in = loader.getResourceAsStream("AndroidManifest.xml")) {
            manifest = in != null && in.read() >= 0;
        } catch (IOException e) {
            manifest = false;
        }
        boolean loads;
        try {
            loads = loader.loadClass("megler.test.hello.Swatch") == Swatch.class && Class.forName("megler.test.hello.Swatch", false, loader) == Swatch.class;
        } catch (ClassNotFoundException e) {
            loads = false;
        }
        boolean missing;
        try {
            loader.loadClass("megler.test.hello.NotThere");
            missing = false;
        } catch (ClassNotFoundException e) {
            missing = true;
        }
        Log.i("HelloTest", "loader=" + loader.getClass().getName() + " same=" + same + " manifest=" + manifest + " loads=" + loads + " missing=" + missing + " system="
                + String.class.getClassLoader().getClass().getName() + " apk=" + new File(getPackageCodePath()).isFile());
    }
}
