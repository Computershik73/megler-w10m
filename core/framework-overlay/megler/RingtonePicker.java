// Megler: new in the overlay. Android's sound picker (RingtoneManager.ACTION_RINGTONE_PICKER, which the system's SoundPicker
// app answers with its RingtonePickerActivity) over the app that asks for it: a dialog of the sounds of the type asked for,
// one of them checked, OK and Cancel; OK gives the caller RESULT_OK and the sound's address in EXTRA_RINGTONE_PICKED_URI,
// Cancel or Back RESULT_CANCELED (megler.AppRunner.startActivity).
//
// Why. Nothing answered the intent: startActivity threw ActivityNotFoundException, which an app does not expect of an
// intent every Android phone answers, and WhatsApp ended when its Settings > Notifications > "Notification tone" was
// tapped (0.1.0.734, a desktop PC).
//
// What it lists, as RingtonePickerActivity does: "Default ..." (EXTRA_RINGTONE_SHOW_DEFAULT, true unless the app says
// otherwise) whose address is EXTRA_RINGTONE_DEFAULT_URI or Settings.System's default of the type; "None"
// (EXTRA_RINGTONE_SHOW_SILENT, likewise), whose address is null; then the sounds of the shared storage that MediaStore
// marks as of the type (is_ringtone, is_notification, is_alarm: files under /sdcard/Ringtones, Notifications, Alarms,
// megler.MediaProvider), as far as the app may see them. The one checked at first is EXTRA_RINGTONE_EXISTING_URI's. The
// title is EXTRA_RINGTONE_TITLE, else the type's. A tap on one of the storage's sounds plays it, as Android's does; the
// default plays nothing, there being no sound of the phone's behind it here (RingtoneManager.getActualDefaultRingtoneUri).
// Not here: Android 9's "Add ringtone", which copies a file the user picks into the storage's Ringtones.
package megler;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.net.Uri;
import java.util.ArrayList;

final class RingtonePicker {
    private RingtonePicker() {}

    /** The one shown now, if any (the tests' "ringtone-picker"). */
    static AlertDialog sShown;

    /** Context.startActivity's way for ACTION_RINGTONE_PICKER: the picker shown, true; false for any other intent. */
    static boolean start(Context from, Intent i, int request) {
        if (!RingtoneManager.ACTION_RINGTONE_PICKER.equals(i.getAction())) return false;
        final Intent intent = new Intent(i);
        // (a window: on the main thread, whichever thread the app started it from)
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) show(from, intent, request);
        else new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> show(from, intent, request));
        return true;
    }

    private static void show(Context from, Intent i, int request) {
        final Activity over = from instanceof Activity && !((Activity) from).isFinishing() ? (Activity) from : AppRunner.top();
        if (over == null) {
            android.util.Log.i("Megler", "sound picker: no screen to show it on, nothing opened");
            return;
        }
        int type = i.getIntExtra(RingtoneManager.EXTRA_RINGTONE_TYPE, -1);
        Uri defaultUri = i.getParcelableExtra(RingtoneManager.EXTRA_RINGTONE_DEFAULT_URI);
        if (defaultUri == null) defaultUri = RingtoneManager.getDefaultUri(type == -1 ? RingtoneManager.TYPE_RINGTONE : type);
        Uri existing = i.getParcelableExtra(RingtoneManager.EXTRA_RINGTONE_EXISTING_URI);
        CharSequence title = i.getCharSequenceExtra(RingtoneManager.EXTRA_RINGTONE_TITLE);
        if (title == null) title = type == RingtoneManager.TYPE_ALARM ? "Alarm sounds" : type == RingtoneManager.TYPE_NOTIFICATION ? "Notification sounds" : "Ringtones";

        final ArrayList<CharSequence> labels = new ArrayList<>();
        final ArrayList<Uri> uris = new ArrayList<>();
        int defaultAt = -1, silentAt = -1;
        if (i.getBooleanExtra(RingtoneManager.EXTRA_RINGTONE_SHOW_DEFAULT, true)) {
            defaultAt = labels.size();
            labels.add(type == RingtoneManager.TYPE_ALARM ? "Default alarm sound" : type == RingtoneManager.TYPE_NOTIFICATION ? "Default notification sound" : "Default ringtone");
            uris.add(defaultUri);
        }
        if (i.getBooleanExtra(RingtoneManager.EXTRA_RINGTONE_SHOW_SILENT, true)) {
            silentAt = labels.size();
            labels.add("None");
            uris.add(null);
        }
        int stored = sounds(over, type, labels, uris);

        int checked = -1;
        if (existing == null) checked = silentAt;
        else if (defaultAt >= 0 && (existing.equals(defaultUri) || RingtoneManager.isDefault(existing))) checked = defaultAt;
        else for (int k = 0; k < uris.size(); k++) if (k != defaultAt && existing.equals(uris.get(k))) checked = k;

        final int[] picked = { checked };
        final boolean[] done = { false };
        final MediaPlayer[] playing = { null };
        final int fDefaultAt = defaultAt;
        boolean night = (over.getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
                        == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        int theme = night ? android.R.style.Theme_DeviceDefault_Dialog_Alert : android.R.style.Theme_DeviceDefault_Light_Dialog_Alert;
        AlertDialog.Builder b = new AlertDialog.Builder(over, theme).setTitle(title)
            .setSingleChoiceItems(labels.toArray(new CharSequence[0]), checked, (d, which) -> {
                picked[0] = which;
                stop(playing);
                Uri u = uris.get(which);
                if (u != null && which != fDefaultAt) playing[0] = play(over, u);
            })
            .setPositiveButton(android.R.string.ok, (d, which) -> {
                if (done[0]) return;
                done[0] = true;
                stop(playing);
                Intent result = new Intent();
                Uri u = picked[0] >= 0 ? uris.get(picked[0]) : null;
                result.putExtra(RingtoneManager.EXTRA_RINGTONE_PICKED_URI, u);
                android.util.Log.i("Megler", "sound picker: " + (picked[0] < 0 ? "nothing picked" : "\"" + labels.get(picked[0]) + "\"") + " given to the app");
                if (request >= 0) over.meglerActivityResult(request, Activity.RESULT_OK, result);
            })
            .setNegativeButton(android.R.string.cancel, (d, which) -> cancelled(over, request, done, playing))
            .setOnCancelListener(d -> cancelled(over, request, done, playing));
        final AlertDialog dialog = b.create();
        dialog.setOnDismissListener(d -> {
            stop(playing);
            if (sShown == dialog) sShown = null;
        });
        sShown = dialog;
        android.util.Log.i("Megler", "sound picker: \"" + title + "\", " + labels.size() + " choice(s), " + stored + " of them the storage's sounds"
                           + (checked >= 0 ? ", \"" + labels.get(checked) + "\" checked" : ""));
        dialog.show();
    }

    private static void cancelled(Activity over, int request, boolean[] done, MediaPlayer[] playing) {
        if (done[0]) return;
        done[0] = true;
        stop(playing);
        android.util.Log.i("Megler", "sound picker: closed, nothing picked");
        if (request >= 0) over.meglerActivityResult(request, Activity.RESULT_CANCELED, null);
    }

    /*
     * The shared storage's sounds of the type (the RingtoneManager type's bits; none or TYPE_ALL every kind), added to the
     * lists by title; how many. What MediaStore does not let the app see is not there, and a refusal leaves none.
     */
    private static int sounds(Context c, int type, ArrayList<CharSequence> labels, ArrayList<Uri> uris) {
        StringBuilder where = new StringBuilder();
        if (type == -1) type = RingtoneManager.TYPE_ALL;
        if ((type & RingtoneManager.TYPE_RINGTONE) != 0) where.append("is_ringtone=1");
        if ((type & RingtoneManager.TYPE_NOTIFICATION) != 0) where.append(where.length() > 0 ? " OR " : "").append("is_notification=1");
        if ((type & RingtoneManager.TYPE_ALARM) != 0) where.append(where.length() > 0 ? " OR " : "").append("is_alarm=1");
        if (where.length() == 0) return 0;
        Uri collection = android.provider.MediaStore.Audio.Media.EXTERNAL_CONTENT_URI;
        int n = 0;
        try (Cursor cur = c.getContentResolver().query(collection, new String[] { "_id", "title" }, where.toString(), null, "title")) {
            while (cur != null && cur.moveToNext()) {
                String t = cur.getString(1);
                labels.add(t != null ? t : "Sound " + cur.getLong(0));
                uris.add(android.content.ContentUris.withAppendedId(collection, cur.getLong(0)));
                n++;
            }
        } catch (RuntimeException e) {
            android.util.Log.i("Megler", "sound picker: the storage's sounds cannot be listed: " + e);
        }
        return n;
    }

    private static MediaPlayer play(Context c, Uri u) {
        MediaPlayer p = new MediaPlayer();
        try {
            p.setDataSource(c, u);
            p.setOnPreparedListener(MediaPlayer::start);
            p.prepareAsync();
            return p;
        } catch (Exception e) {
            android.util.Log.i("Megler", "sound picker: " + u + " cannot be played: " + e);
            p.release();
            return null;
        }
    }

    private static void stop(MediaPlayer[] playing) {
        if (playing[0] == null) return;
        try { playing[0].release(); } catch (RuntimeException e) { /* gone already */ }
        playing[0] = null;
    }
}
