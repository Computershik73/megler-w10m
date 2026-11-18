package megler.test.plugin;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcelable;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.TreeSet;

import megler.test.shared.Note;
import megler.test.shared.Tag;

/**
 * The activity another app asks for (the test app crossapp, as Total Commander asks its Wi-Fi plug-in's WifiSendActivity):
 * it says the Intent it was started with, value by value -- what the other app wrote is what the test expects here
 * (core/tests/mg_java_tests.c, run_plugin) -- and finishes, which ends the app: nothing else of it was opened.
 */
public class Shown extends Activity {
    private static final String T = "PluginTest";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView v = new TextView(this);
        v.setText("Shown");
        setContentView(v);
        Intent i = getIntent();
        Log.i(T, "shown action=" + i.getAction() + " data=" + i.getData() + " type=" + i.getType() + " component="
                 + (i.getComponent() == null ? null : i.getComponent().flattenToShortString()) + " package=" + i.getPackage());
        Log.i(T, "shown flags=0x" + Integer.toHexString(i.getFlags()) + " categories=" + (i.getCategories() == null ? null : new TreeSet<>(i.getCategories())));
        Bundle e = i.getExtras();
        Log.i(T, "shown keys=" + (e == null ? null : new TreeSet<>(e.keySet())));
        Bundle nested = i.getBundleExtra("nested");
        StringBuilder wide = new StringBuilder();
        String w = i.getStringExtra("wide");
        for (int k = 0; w != null && k < w.length(); k += Character.charCount(w.codePointAt(k))) wide.append(k > 0 ? " " : "").append(Integer.toHexString(w.codePointAt(k)));
        Log.i(T, "shown text=" + i.getStringExtra("text") + " wide=" + wide + " count=" + i.getIntExtra("count", -1) + " big=" + i.getLongExtra("big", -1) + " flag="
                 + i.getBooleanExtra("flag", false) + " ratio=" + i.getDoubleExtra("ratio", -1) + " longs=" + Arrays.toString(i.getLongArrayExtra("longs")) + " bytes="
                 + Arrays.toString(i.getByteArrayExtra("bytes")) + " strings=" + Arrays.toString(i.getStringArrayExtra("strings")) + " nested="
                 + (nested == null ? null : nested.getString("inner") + "/" + nested.getInt("n") + "/" + nested.getStringArrayList("list")));
        ArrayList<Uri> uris = i.getParcelableArrayListExtra("uris");
        Log.i(T, "shown uris=" + uris + " uri=" + (uris != null && !uris.isEmpty() && ((Object) uris.get(0)) instanceof Uri) + " stream="
                 + i.getParcelableExtra(Intent.EXTRA_STREAM));
        Object note = i.getParcelableExtra("note"), tag = i.getSerializableExtra("tag");
        Parcelable[] notes = i.getParcelableArrayExtra("notes");
        Log.i(T, "shown note=" + note + " mine=" + (note instanceof Note) + " notes=" + Arrays.toString(notes) + " tag=" + tag + " mine=" + (tag instanceof Tag));
        Log.i(T, "shown only=" + (e == null ? null : e.get("only")) + " binder=" + (e == null ? null : e.get("binder")) + " inner="
                 + (i.getParcelableExtra("inner") instanceof Intent ? ((Intent) i.getParcelableExtra("inner")).getAction() + "/" + ((Intent) i.getParcelableExtra("inner")).getStringExtra("deep") : null));
        // The file the other app wrote this Intent into is read once and gone; what is left is another's (the first request
        // of the two the crossapp app made, which no run of this app was started with).
        String[] left = new File("/data/megler-services/intents/" + getPackageName()).list();
        Log.i(T, "shown left=" + (left == null ? -1 : left.length) + " application=" + getApplication().getClass().getName() + " created=" + App.created);
        // Its answer, for the app that asked for a result (megler.AppResults writes it where that app reads it).
        setResult(RESULT_OK, new Intent(getPackageName() + ".DONE").putExtra("answer", "yes").putParcelableArrayListExtra("uris", uris));
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "shown onDestroy");
    }
}
