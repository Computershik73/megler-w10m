package megler.test.splits;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Locale;
import java.util.zip.ZipFile;

/**
 * An app whose base is not whole: its native library, its pictures for each density, its strings for each language, more code
 * and an asset pack are in its split APKs (core/tests/app/splits/splits). Each step is a line of the log, "name ok" or
 * "name FAILED" and why, and the facts the test compares with what Android would say ("split-names ..."). The picture that
 * only the split for its density has is shown, for the test to find in the window.
 */
public class Main extends Activity {
    private static final String T = "SplitsTest";
    private int ok, failed;
    private String detail;

    static native int answer();

    private interface Step {
        boolean run() throws Throwable;
    }

    private void step(String name, Step s) {
        detail = null;
        boolean good;
        try {
            good = s.run();
        } catch (Throwable t) {
            good = false;
            detail = t.toString();
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED" + (detail == null ? "" : ": " + detail)) + (good && detail != null ? " (" + detail + ")" : ""));
    }

    private static String read(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[256];
        for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n);
        in.close();
        return new String(out.toByteArray(), "UTF-8");
    }

    private String greetingIn(String language) {
        Configuration c = new Configuration(getResources().getConfiguration());
        c.setLocale(new Locale(language));
        return new Resources(getAssets(), getResources().getDisplayMetrics(), c).getString(R.string.greeting);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        final ApplicationInfo ai = getApplicationInfo();

        // What Android would say of the splits: their names, sorted, and each one's file beside the base under Android's name.
        Log.i(T, "split-names " + (ai.splitNames == null ? "none" : String.join(",", ai.splitNames)));
        Log.i(T, "source " + ai.sourceDir + " splits " + (ai.splitSourceDirs == null ? "none" : String.join(",", ai.splitSourceDirs)));
        step("split-dirs", () -> {
            if (ai.splitNames == null || ai.splitSourceDirs == null || ai.splitNames.length != ai.splitSourceDirs.length) return false;
            String[] sorted = ai.splitNames.clone();
            Arrays.sort(sorted);
            if (!Arrays.equals(sorted, ai.splitNames)) return false;
            String dir = new File(ai.sourceDir).getParent();
            for (int i = 0; i < ai.splitNames.length; i++) {
                File f = new File(ai.splitSourceDirs[i]);
                if (!f.getName().equals("split_" + ai.splitNames[i] + ".apk") || !dir.equals(f.getParent()) || !f.isFile()) {
                    detail = ai.splitSourceDirs[i];
                    return false;
                }
                // Each is an APK an app can open as one.
                try (ZipFile z = new ZipFile(f)) {
                    if (z.getEntry("AndroidManifest.xml") == null) return false;
                }
            }
            return dir.equals("/data/app/megler.test.splits-1");
        });
        step("package-info", () -> {
            PackageInfo p = getPackageManager().getPackageInfo(getPackageName(), 0);
            return Arrays.equals(p.splitNames, ai.splitNames);
        });
        step("code-path", () -> getPackageCodePath().equals(ai.sourceDir) && new File(getPackageCodePath()).isFile());

        // The native library, which only the split for 32-bit ARM has.
        step("native", () -> {
            System.loadLibrary("splitlib");
            int a = answer();
            detail = "answer=" + a;
            return a == 4242 && new File(ai.nativeLibraryDir, "libsplitlib.so").isFile();
        });

        // A picture only the split for its density has (xxhdpi, 24 x 24 there), scaled to this screen's density as Android does.
        final Drawable dense = getResources().getDrawable(R.drawable.dense);
        step("drawable", () -> {
            Bitmap b = ((BitmapDrawable) dense).getBitmap();
            int want = Math.round(24f * getResources().getDisplayMetrics().densityDpi / 480f), w = dense.getIntrinsicWidth();
            int px = b.getPixel(b.getWidth() / 2, b.getHeight() / 2) & 0xFFFFFF;
            detail = "w=" + w + " want=" + want + " color=" + Integer.toHexString(px);
            return Math.abs(w - want) <= 1 && px == 0xFF00FF;
        });
        step("drawable-mdpi", () -> {
            Bitmap b = ((BitmapDrawable) getResources().getDrawable(R.drawable.low)).getBitmap();
            return (b.getPixel(b.getWidth() / 2, b.getHeight() / 2) & 0xFFFFFF) == 0x00FFFF;
        });

        // Strings only the language splits have: English for this phone, Russian when asked for, and German left out (the base's).
        step("string", () -> {
            detail = getString(R.string.greeting);
            return "from config.en".equals(detail);
        });
        step("string-ru", () -> {
            detail = greetingIn("ru");
            return "из config.ru".equals(detail);
        });
        step("string-de", () -> {
            detail = greetingIn("de");
            return "from the base".equals(detail);
        });

        // Assets: the base's, and an asset pack's, found and listed through the same AssetManager.
        step("asset-base", () -> "a file of the base's\n".equals(read(getAssets().open("base.txt"))));
        step("asset-pack", () -> {
            String text = read(getAssets().open("pack/hello.txt"));
            String[] in = getAssets().list("pack"), top = getAssets().list("");
            detail = "pack=" + Arrays.toString(in) + " top=" + Arrays.toString(top);
            return "hello from the pack split\n".equals(text) && Arrays.asList(in).contains("hello.txt") && Arrays.asList(top).contains("base.txt")
                && Arrays.asList(top).contains("pack");
        });

        // Code only the feature split has, through the app's own class loader.
        step("feature", () -> {
            Class<?> c = Class.forName("megler.test.splits.extra.Extra");
            String said = (String) c.getMethod("hello").invoke(null);
            detail = said;
            return "hello from the extra split".equals(said) && c.getClassLoader() == getClassLoader();
        });

        // The expansion file an .xapk brings, where Android puts it: the app's own Android/obb.
        step("obb", () -> {
            File f = new File(getObbDir(), "main.7.megler.test.splits.obb");
            Log.i(T, "obb " + (f.isFile() ? "present" : "absent") + " in " + getObbDir());
            if (!f.isFile()) return true;
            return "the expansion file of megler.test.splits\n".equals(read(new FileInputStream(f)));
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setBackgroundColor(0xFF202020);
        TextView text = new TextView(this);
        text.setText(getString(R.string.greeting));
        text.setTextColor(0xFFFFFFFF);
        column.addView(text);
        final ImageView image = new ImageView(this);
        image.setImageDrawable(dense);
        column.addView(image, new LinearLayout.LayoutParams(dense.getIntrinsicWidth(), dense.getIntrinsicHeight()));
        image.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                int[] at = new int[2];
                v.getLocationInWindow(at);
                Log.i(T, "layout picture=" + at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight());
            }
        });
        setContentView(column);
    }

    @Override
    protected void onDestroy() {
        Log.i(T, "onDestroy");
        super.onDestroy();
    }
}
