// Megler: the screens of Android's Settings that an app opens by an intent, which Megler, having no Settings app, shows
// itself, on top of the app that asks (megler.AppRunner.startActivity):
//  - the accounts (android.settings.SYNC_SETTINGS, ACCOUNT_SYNC_SETTINGS, ADD_ACCOUNT_SETTINGS): the accounts of the types
//    asked for (Settings.EXTRA_ACCOUNT_TYPES, else all), each with Remove account -- AccountManager.removeAccount, where the
//    account's authenticator says whether it may go -- and Add account for each type. microG has no removal of its own:
//    tapping an account in its settings opens Android's accounts screen, which had opened nothing here.
//  - the app's info (android.settings.APPLICATION_DETAILS_SETTINGS for the app's own package): its name, version and
//    package, Clear storage -- the core clears its data when it next starts (megler_ask_clear_data), and the app ends now,
//    as Android stops an app whose data it clears --, Clear cache -- the core clears what is in its cache folders when it
//    next starts (megler_ask_clear_cache), the app going on meanwhile -- Force stop, and Open by default.
//  - the app's links (android.settings.APP_OPEN_BY_DEFAULT_SETTINGS for its own package, Android's "Open by default"): which
//    web links it opens (megler.Defaults, links.<package>): those verified for it, every one its filters name, or none; its
//    sites, each verified or not as last found (megler.AssetLinks); and Clear defaults, its "Always" answers gone. An app
//    whose links are not verified sends the user there to allow them.
//  - the default apps (android.settings.MANAGE_DEFAULT_APPS_SETTINGS): the browser web links open in -- Windows' own, or a
//    browser app -- and every "Always" answer of "Open with", each to be forgotten (megler.Resolver, megler.Defaults).
// They are made from their classes, not by the app's component factory: on Android they are the Settings app's.
package megler;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.Arrays;

public final class SystemScreens {
    private SystemScreens() {}

    static final String ACCOUNTS = Accounts.class.getName(), APP_INFO = AppInfo.class.getName(), OPEN_BY_DEFAULT = OpenByDefault.class.getName(),
                        DEFAULT_APPS = DefaultApps.class.getName();

    /** The screen of these that `i` asks for, by class, or null. */
    static String screenFor(Intent i) {
        String action = i.getAction();
        if (action == null) return null;
        switch (action) {
            case "android.settings.SYNC_SETTINGS":
            case "android.settings.ACCOUNT_SYNC_SETTINGS":
            case "android.settings.ADD_ACCOUNT_SETTINGS":
                return ACCOUNTS;
            case "android.settings.APPLICATION_DETAILS_SETTINGS":
                return own(i) ? APP_INFO : null;
            case "android.settings.APP_OPEN_BY_DEFAULT_SETTINGS":
                return own(i) ? OPEN_BY_DEFAULT : null;
            case "android.settings.MANAGE_DEFAULT_APPS_SETTINGS":
                return DEFAULT_APPS;
            default:
                return null;
        }
    }

    /* An intent for the app's own package (package:<it>). */
    private static boolean own(Intent i) {
        android.net.Uri u = i.getData();
        return u != null && "package".equals(u.getScheme()) && Native.packageName().equals(u.getSchemeSpecificPart());
    }

    static boolean isScreen(String cls) { return ACCOUNTS.equals(cls) || APP_INFO.equals(cls) || OPEN_BY_DEFAULT.equals(cls) || DEFAULT_APPS.equals(cls); }

    /** What the manifest would say of such an activity: the platform's theme, no more. */
    static Manifest.Component component(String cls) {
        Manifest.Component c = new Manifest.Component();
        c.name = cls;
        c.kind = "activity";
        c.theme = android.R.style.Theme_DeviceDefault;
        return c;
    }

    // ---- what both screens are made of

    private static int dp(Activity a, float n) { return Math.round(n * a.getResources().getDisplayMetrics().density); }

    private static TextView text(Activity a, CharSequence s, float sp, boolean bold, float alpha) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(sp);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setAlpha(alpha);
        return t;
    }

    private static LinearLayout page(Activity a, String title) {
        a.setTitle(title);
        ScrollView scroll = new ScrollView(a);
        LinearLayout column = new LinearLayout(a);
        column.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(a, 16);
        column.setPadding(pad, pad, pad, pad);
        scroll.addView(column, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        a.setContentView(scroll);
        return column;
    }

    private static void add(Activity a, LinearLayout column, View v, float top) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(a, top);
        column.addView(v, lp);
    }

    private static Button button(Activity a, String label, View.OnClickListener click) {
        Button b = new Button(a);
        b.setText(label);
        b.setOnClickListener(click);
        return b;
    }

    // ---- the accounts

    public static final class Accounts extends Activity {
        private LinearLayout column;

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            column = page(this, "Accounts");
        }

        @Override
        protected void onResume() {
            super.onResume();
            fill();
        }

        /** The types asked for (Settings.EXTRA_ACCOUNT_TYPES, or the one account's of ACCOUNT_SYNC_SETTINGS), or null for all. */
        private String[] types() {
            Intent i = getIntent();
            String[] t = i.getStringArrayExtra("account_types");
            if (t != null && t.length > 0) return t;
            Account one = i.getParcelableExtra("account");
            return one != null ? new String[] { one.type } : null;
        }

        private void fill() {
            column.removeAllViews();
            AccountManager am = AccountManager.get(this);
            String[] types = types();
            AuthenticatorDescription[] all = am.getAuthenticatorTypes();
            add(this, column, text(this, "Accounts", 22, true, 1f), 0);
            ArrayList<Account> shown = new ArrayList<>();
            for (Account a : am.getAccounts()) if (types == null || Arrays.asList(types).contains(a.type)) shown.add(a);
            if (shown.isEmpty()) add(this, column, text(this, "No accounts.", 15, false, 0.7f), 16);
            for (Account a : shown) {
                add(this, column, text(this, a.name, 17, false, 1f), 20);
                add(this, column, text(this, labelOf(all, a.type), 14, false, 0.7f), 2);
                Button remove = button(this, "Remove account", v -> confirmRemove(a));
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                lp.topMargin = dp(this, 8);
                column.addView(remove, lp);
            }
            int addable = 0;
            for (AuthenticatorDescription d : all) {
                if (types != null && !Arrays.asList(types).contains(d.type)) continue;
                addable++;
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                lp.topMargin = dp(this, 28);
                column.addView(button(this, "Add " + labelOf(all, d.type) + " account", v -> am.addAccount(d.type, null, null, null, this, f -> fill(), null)), lp);
            }
            android.util.Log.i("Megler", "accounts screen: " + shown.size() + " account(s)" + (types == null ? "" : " of " + String.join(",", types)) + ", "
                    + addable + " type(s) to add");
        }

        private String labelOf(AuthenticatorDescription[] all, String type) {
            for (AuthenticatorDescription d : all) {
                if (!d.type.equals(type)) continue;
                try {
                    CharSequence label = d.labelId != 0 ? createPackageContext(d.packageName, 0).getResources().getText(d.labelId) : null;
                    if (label != null && label.length() > 0) return label.toString();
                } catch (Exception e) {
                    // the authenticator's own label cannot be read: its type says it
                }
            }
            return "com.google".equals(type) ? "Google" : type;
        }

        private void confirmRemove(Account a) {
            new AlertDialog.Builder(this)
                    .setTitle("Remove account?")
                    .setMessage("Removing " + a.name + " deletes it, and what apps keep with it, from every app here.")
                    .setPositiveButton("Remove account", (d, w) -> remove(a))
                    .setNegativeButton("Cancel", null)
                    .show();
        }

        private void remove(Account a) {
            AccountManager.get(this).removeAccount(a, this, f -> {
                String said;
                try {
                    said = f.getResult().getBoolean(AccountManager.KEY_BOOLEAN_RESULT) ? "Account removed." : "This account cannot be removed.";
                } catch (Exception e) {
                    said = "The account could not be removed: " + e.getMessage();
                }
                android.util.Log.i("Megler", "accounts screen: " + said);
                Toast.makeText(this, said, Toast.LENGTH_LONG).show();
                fill();
            }, null);
        }
    }

    // ---- the app's info

    public static final class AppInfo extends Activity {
        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            LinearLayout column = page(this, "App info");
            PackageManager pm = getPackageManager();
            String pkg = getPackageName(), version = "";
            try {
                PackageInfo p = pm.getPackageInfo(pkg, 0);
                version = p.versionName != null ? p.versionName : String.valueOf(p.versionCode);
            } catch (Exception e) {
                // no version to say
            }
            LinearLayout head = new LinearLayout(this);
            head.setGravity(Gravity.CENTER_VERTICAL);
            ImageView icon = new ImageView(this);
            try { icon.setImageDrawable(getApplicationInfo().loadIcon(pm)); } catch (Exception e) { /* no icon */ }
            head.addView(icon, new LinearLayout.LayoutParams(dp(this, 56), dp(this, 56)));
            LinearLayout names = new LinearLayout(this);
            names.setOrientation(LinearLayout.VERTICAL);
            names.setPadding(dp(this, 16), 0, 0, 0);
            names.addView(text(this, getApplicationInfo().loadLabel(pm), 22, true, 1f));
            names.addView(text(this, pkg, 14, false, 0.7f));
            if (!version.isEmpty()) names.addView(text(this, "Version " + version, 14, false, 0.7f));
            head.addView(names);
            add(this, column, head, 0);

            add(this, column, button(this, "Force stop", v -> new AlertDialog.Builder(this)
                    .setTitle("Force stop?")
                    .setMessage("If you force stop an app, it may misbehave.")
                    .setPositiveButton("OK", (d, w) -> AppRunner.finishAll())
                    .setNegativeButton("Cancel", null)
                    .show()), 28);
            add(this, column, button(this, "Clear storage", v -> new AlertDialog.Builder(this)
                    .setTitle("Clear app storage?")
                    .setMessage("All this app's data will be deleted permanently: its files, settings, accounts and databases. "
                            + "The app closes now, and its data goes before it next starts."
                            // (Google's own Play services keep the Google accounts' sign-in in their data: megler.Accounts.)
                            + (GooglePackages.GMS.equals(getPackageName()) && !MicroG.isMicroG()
                                    ? " Your Google accounts are signed in through this data: they are removed with it, and you sign in again afterwards." : ""))
                    .setPositiveButton("Delete", (d, w) -> clear())
                    .setNegativeButton("Cancel", null)
                    .show()), 8);
            add(this, column, button(this, "Clear cache", v -> clearCache()), 8);
            add(this, column, button(this, "Open by default", v -> startActivity(new Intent("android.settings.APP_OPEN_BY_DEFAULT_SETTINGS",
                    android.net.Uri.fromParts("package", getPackageName(), null)))), 8);
            add(this, column, text(this, "Clear storage deletes the app's private files and its Android/data folder; "
                    + "its Android/obb files stay, as on Android. Clear cache deletes what is in its cache folders when it next "
                    + "starts; its files, settings and accounts stay.", 13, false, 0.7f), 16);
        }

        private void clearCache() {
            boolean asked = Native.askClearCache();
            android.util.Log.i("Megler", "app info: clear cache asked" + (asked ? "" : ", and the ask could not be written"));
            Toast.makeText(this, asked ? "The app's cache is cleared the next time it starts." : "The app's cache could not be cleared.",
                    Toast.LENGTH_LONG).show();
        }

        private void clear() {
            if (!Native.askClearData()) {
                Toast.makeText(this, "The app's data could not be cleared.", Toast.LENGTH_LONG).show();
                return;
            }
            android.util.Log.i("Megler", "app info: clear storage asked; the app ends now");
            AppRunner.finishAll();
        }
    }

    // ---- the app's links

    public static final class OpenByDefault extends Activity {
        private LinearLayout column;

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            column = page(this, "Open by default");
            fill();
        }

        private void fill() {
            column.removeAllViews();
            String pkg = getPackageName(), mode = Defaults.get(Defaults.LINKS + pkg);
            add(this, column, text(this, "Open supported links", 20, true, 1f), 0);
            android.widget.RadioGroup group = new android.widget.RadioGroup(this);
            String[][] choices = { { null, "Verified links", "Links of the sites that say this app is theirs" },
                                   { "all", "All supported links", "Every link of the sites this app names, verified or not" },
                                   { "off", "Don't open links", "Its links open in the browser" } };
            for (String[] c : choices) {
                android.widget.RadioButton b = new android.widget.RadioButton(this);
                b.setText(c[1] + "\n" + c[2]);
                b.setChecked(c[0] == null ? mode == null : c[0].equals(mode));
                b.setOnClickListener(v -> {
                    Defaults.set(Defaults.LINKS + pkg, c[0]);
                    android.util.Log.i("Megler", "open by default: " + pkg + ": " + c[1]);
                    fill();
                });
                group.addView(b);
            }
            add(this, column, group, 8);
            java.util.LinkedHashMap<String, Boolean> domains = Resolver.domains(Resolver.ownFilters());
            add(this, column, text(this, "Supported links", 20, true, 1f), 24);
            if (domains.isEmpty()) add(this, column, text(this, "This app names no site's links.", 15, false, 0.7f), 8);
            for (java.util.Map.Entry<String, Boolean> d : domains.entrySet()) {
                Boolean verified = d.getValue() ? Resolver.verified(pkg, d.getKey()) : Boolean.FALSE;
                add(this, column, text(this, d.getKey(), 16, false, 1f), 12);
                add(this, column, text(this, !d.getValue() ? "Not verified: the app does not ask to be"
                                           : verified == null ? "Not checked yet: it is when one of its links is opened"
                                           : verified ? "Verified" : "Not verified: the site does not name this app", 13, false, 0.7f), 2);
            }
            int always = 0;
            for (java.util.Map.Entry<String, String> e : Defaults.all().entrySet())
                if (e.getKey().startsWith(Defaults.ALWAYS) && e.getValue().startsWith(pkg + "/")) always++;
            add(this, column, text(this, "Defaults", 20, true, 1f), 24);
            add(this, column, text(this, always == 0 ? "No defaults set." : "Opens " + always + " kind(s) of link or file by default (\"Always\").", 15, false, 0.7f), 8);
            Button clear = button(this, "Clear defaults", v -> {
                int n = Defaults.remove(Defaults.ALWAYS, pkg);
                android.util.Log.i("Megler", "open by default: " + n + " \"Always\" answer(s) for " + pkg + " cleared");
                fill();
            });
            clear.setEnabled(always > 0);
            add(this, column, clear, 8);
        }
    }

    // ---- the default apps

    public static final class DefaultApps extends Activity {
        private LinearLayout column;

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            column = page(this, "Default apps");
            fill();
        }

        private void fill() {
            column.removeAllViews();
            add(this, column, text(this, "Browser app", 20, true, 1f), 0);
            String chosen = Defaults.get(Defaults.BROWSER);
            android.widget.RadioGroup group = new android.widget.RadioGroup(this);
            for (Resolver.Target t : Resolver.browsers()) {
                android.widget.RadioButton b = new android.widget.RadioButton(this);
                b.setText(t.windows() ? "Windows' browser" : String.valueOf(t.label()));
                b.setChecked(t.windows() ? chosen == null || chosen.isEmpty() || Defaults.WINDOWS.equals(chosen) : t.id().equals(chosen));
                b.setOnClickListener(v -> {
                    Defaults.set(Defaults.BROWSER, t.windows() ? null : t.id());
                    android.util.Log.i("Megler", "default apps: the browser is " + t);
                    fill();
                });
                group.addView(b);
            }
            add(this, column, group, 8);
            add(this, column, text(this, "Opened with \"Always\"", 20, true, 1f), 24);
            int n = 0;
            for (java.util.Map.Entry<String, String> e : Defaults.all().entrySet()) {
                if (!e.getKey().startsWith(Defaults.ALWAYS)) continue;
                n++;
                String key = e.getKey();
                add(this, column, text(this, Resolver.kindInWords(key.substring(Defaults.ALWAYS.length())), 16, false, 1f), 12);
                add(this, column, text(this, Resolver.labelOf(e.getValue()), 13, false, 0.7f), 2);
                add(this, column, button(this, "Forget", v -> {
                    Defaults.set(key, null);
                    fill();
                }), 4);
            }
            if (n == 0) add(this, column, text(this, "None: \"Open with\" asks each time several apps can open something.", 15, false, 0.7f), 8);
        }
    }
}
