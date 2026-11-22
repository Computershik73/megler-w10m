// Megler: "Open with", Android's ResolverActivity, over the app that started an intent several apps take (megler.Resolver):
// each, its icon and its name, and the package under a name two have; a tap picks one, "Just Once" opens it, "Always" opens
// it and has every intent of that kind go to it from then on (megler.Defaults, which Megler's Settings and Android's screen of
// default apps let the user take back). With `always` false (Intent.createChooser's) a tap opens it and nothing is kept.
// Back or a tap outside closes it and nothing opens. It is in the phone's light or dark, as the app's Configuration says.
package megler;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.List;

final class Chooser {
    private Chooser() {}

    interface Chosen { void chose(Resolver.Target t, boolean always); }

    /** The one shown now, if any (the log's, and the tests' "link-chooser"). */
    static AlertDialog sShown;

    static void show(Activity over, List<Resolver.Target> targets, boolean always, Chosen chosen, Runnable closed) {
        boolean night = (over.getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
                        == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        int theme = night ? android.R.style.Theme_DeviceDefault_Dialog_Alert : android.R.style.Theme_DeviceDefault_Light_Dialog_Alert;
        float dp = over.getResources().getDisplayMetrics().density;
        final int[] picked = { -1 };
        final boolean[] done = { false };
        LinearLayout list = new LinearLayout(over);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, Math.round(8 * dp), 0, Math.round(8 * dp));
        final View[] rows = new View[targets.size()];
        final AlertDialog[] dialog = new AlertDialog[1];
        int selected = night ? Color.argb(0x33, 0xff, 0xff, 0xff) : Color.argb(0x1f, 0, 0, 0);
        for (int k = 0; k < targets.size(); k++) {
            Resolver.Target t = targets.get(k);
            LinearLayout row = new LinearLayout(over);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setMinimumHeight(Math.round(56 * dp));
            row.setPadding(Math.round(24 * dp), Math.round(6 * dp), Math.round(24 * dp), Math.round(6 * dp));
            ImageView icon = new ImageView(over);
            android.graphics.drawable.Drawable d = t.icon();
            if (d != null) icon.setImageDrawable(d);
            else icon.setImageDrawable(new ColorDrawable(night ? 0xff5f6368 : 0xffdadce0));
            LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(Math.round(36 * dp), Math.round(36 * dp));
            ip.rightMargin = Math.round(20 * dp);
            row.addView(icon, ip);
            LinearLayout texts = new LinearLayout(over);
            texts.setOrientation(LinearLayout.VERTICAL);
            TextView name = new TextView(over);
            name.setText(t.label());
            name.setTextSize(16);
            texts.addView(name);
            String second = t.windows() ? "Windows' own browser" : sameLabel(targets, t) ? t.pkg : null;
            if (second != null) {
                TextView sub = new TextView(over);
                sub.setText(second);
                sub.setTextSize(13);
                sub.setAlpha(0.7f);
                texts.addView(sub);
            }
            row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            final int index = k;
            row.setOnClickListener(v -> {
                if (!always) {
                    finish(dialog[0], done);
                    chosen.chose(targets.get(index), false);
                    return;
                }
                if (picked[0] == index) {
                    // a second tap on the one picked opens it, as Android's
                    finish(dialog[0], done);
                    chosen.chose(targets.get(index), false);
                    return;
                }
                if (picked[0] >= 0) rows[picked[0]].setBackgroundColor(Color.TRANSPARENT);
                picked[0] = index;
                rows[index].setBackgroundColor(selected);
                if (dialog[0] != null) {
                    Button once = dialog[0].getButton(AlertDialog.BUTTON_NEGATIVE), ever = dialog[0].getButton(AlertDialog.BUTTON_POSITIVE);
                    if (once != null) once.setEnabled(true);
                    if (ever != null) ever.setEnabled(true);
                }
            });
            rows[k] = row;
            list.addView(row, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }
        ScrollView scroll = new ScrollView(over);
        scroll.addView(list);
        AlertDialog.Builder b = new AlertDialog.Builder(over, theme).setTitle("Open with").setView(scroll);
        if (always) {
            b.setNegativeButton("Just Once", null);
            b.setPositiveButton("Always", null);
        }
        b.setOnCancelListener(x -> {
            if (!done[0]) {
                done[0] = true;
                closed.run();
            }
        });
        dialog[0] = b.create();
        dialog[0].setOnDismissListener(x -> { if (sShown == dialog[0]) sShown = null; });
        sShown = dialog[0];
        dialog[0].show();
        if (always) {
            Button once = dialog[0].getButton(AlertDialog.BUTTON_NEGATIVE), ever = dialog[0].getButton(AlertDialog.BUTTON_POSITIVE);
            if (once != null) {
                once.setEnabled(false);
                once.setOnClickListener(v -> {
                    if (picked[0] < 0) return;
                    finish(dialog[0], done);
                    chosen.chose(targets.get(picked[0]), false);
                });
            }
            if (ever != null) {
                ever.setEnabled(false);
                ever.setOnClickListener(v -> {
                    if (picked[0] < 0) return;
                    finish(dialog[0], done);
                    chosen.chose(targets.get(picked[0]), true);
                });
            }
        }
    }

    private static void finish(AlertDialog d, boolean[] done) {
        done[0] = true;
        if (d != null) d.dismiss();
    }

    private static boolean sameLabel(List<Resolver.Target> targets, Resolver.Target t) {
        String l = String.valueOf(t.label());
        for (Resolver.Target x : targets) if (x != t && String.valueOf(x.label()).equals(l)) return true;
        return false;
    }
}
