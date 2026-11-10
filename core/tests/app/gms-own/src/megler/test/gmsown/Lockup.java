package megler.test.gmsown;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.ContextThemeWrapper;
import com.google.android.gms.R;
import java.util.Arrays;
import java.util.Locale;

/**
 * An activity of Google Play services' that an app starts and that runs in the app, as their account settings do ("Manage
 * your Google Account": SettingsLoaderActivity, then a Compose MainActivity whose ProductLockupView loads the Google logo).
 * It reads their resources as those screens do: a picture whose every value is a density's (gms_lockup, in the density's
 * split alone), a word whose English is in the English split alone, and both again through a context of another
 * configuration, as AppCompat's attachBaseContext2 wraps an activity's base where the app's night mode or locales differ
 * (a ContextThemeWrapper with an override configuration, whose resources are createConfigurationContext's). And whether its
 * theme has an attribute only the app's theme sets. Said in one line, and the activity finishes.
 */
public class Lockup extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String own = picture(getResources()) + " word=" + word(getResources());
        ContextThemeWrapper wrapped = new ContextThemeWrapper(this, android.R.style.Theme_DeviceDefault);
        Configuration override = new Configuration();
        override.setLocale(Locale.US);
        wrapped.applyOverrideConfiguration(override);
        Resources other = wrapped.getResources();
        String label;
        try {
            label = other.getString(R.string.gms_own_label);
        } catch (Resources.NotFoundException e) {
            label = "missing";
        }
        ApplicationInfo info = getApplicationInfo();
        Log.i("GmsOwnTest", "lockup " + own + " wrapped " + picture(other) + " word=" + word(other) + " label=" + label
            + " splits=" + (info.splitNames == null ? "none" : Arrays.toString(info.splitNames)) + " host theme attribute " + hostAttribute()
            + " theme " + themeId());
        finish();
    }

    /* The theme this activity was given (Android's hidden getThemeResId): it names none, nor does its application. */
    private String themeId() {
        try {
            return "0x" + Integer.toHexString((Integer) android.content.Context.class.getMethod("getThemeResId").invoke(this));
        } catch (ReflectiveOperationException e) {
            return "unknown";
        }
    }

    /*
     * Whether this activity's theme has the attribute that only the app's own theme sets (gms-host's gms_host_mark, found by its
     * name in the app's resources): an activity of theirs begins from its base context's theme (ContextThemeWrapper), which
     * on Android is their package's, never the app's.
     */
    private String hostAttribute() {
        try {
            int mark = getPackageManager().getResourcesForApplication("megler.test.gmshost").getIdentifier("gms_host_mark", "attr", "megler.test.gmshost");
            if (mark == 0) return "unnamed";
            return String.valueOf(getTheme().resolveAttribute(mark, new TypedValue(), true));
        } catch (PackageManager.NameNotFoundException e) {
            return "no host";
        }
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
