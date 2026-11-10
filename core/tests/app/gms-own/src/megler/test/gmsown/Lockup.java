package megler.test.gmsown;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.Log;
import com.google.android.gms.R;
import java.util.Arrays;

/**
 * An activity of Google Play services' that an app starts and that runs in the app, as their account settings do ("Manage
 * your Google Account": SettingsLoaderActivity, then a Compose MainActivity whose ProductLockupView loads the Google logo).
 * It reads their resources as those screens do: a picture whose every value is a density's (gms_lockup, in the density's
 * split alone) and a word whose English is in the English split alone. Said in one line, and the activity finishes.
 */
public class Lockup extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String own = picture(getResources()) + " word=" + word(getResources());
        ApplicationInfo info = getApplicationInfo();
        Log.i("GmsOwnTest", "lockup " + own + " splits=" + (info.splitNames == null ? "none" : Arrays.toString(info.splitNames)));
        finish();
    }

    private String picture(Resources r) {
        try {
            Drawable d = r.getDrawable(R.drawable.gms_lockup, null);
            return d.getIntrinsicWidth() + "x" + d.getIntrinsicHeight();
        } catch (Resources.NotFoundException e) {
            return "missing";
        }
    }

    private static String word(Resources r) {
        try {
            return r.getString(R.string.gms_split_word);
        } catch (Resources.NotFoundException e) {
            return "missing";
        }
    }
}
