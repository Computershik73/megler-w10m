// Overlay of core/framework/gen-src/android/media/RingtoneManager.java (tools/build-framework.sh).
// It differs in getDefaultUri, getDefaultType and isDefault, which are Android's: the generated stub's getDefaultUri gave
// null for every type, and an app that writes the default sound's address into its settings (4PDA in its Application's
// attachBaseContext, Uri.toString) stopped at its start with a NullPointerException. The default addresses are
// Settings.System's, as on Android; the rest is the generated stub's (no sounds: getRingtone gives null, as Android does when
// it cannot open one).
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class RingtoneManager {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final java.lang.String ACTION_RINGTONE_PICKER = "android.intent.action.RINGTONE_PICKER";
    public static final java.lang.String EXTRA_RINGTONE_AUDIO_ATTRIBUTES_FLAGS = "android.intent.extra.ringtone.AUDIO_ATTRIBUTES_FLAGS";
    public static final java.lang.String EXTRA_RINGTONE_DEFAULT_URI = "android.intent.extra.ringtone.DEFAULT_URI";
    public static final java.lang.String EXTRA_RINGTONE_EXISTING_URI = "android.intent.extra.ringtone.EXISTING_URI";
    public static final java.lang.String EXTRA_RINGTONE_INCLUDE_DRM = "android.intent.extra.ringtone.INCLUDE_DRM";
    public static final java.lang.String EXTRA_RINGTONE_PICKED_URI = "android.intent.extra.ringtone.PICKED_URI";
    public static final java.lang.String EXTRA_RINGTONE_SHOW_DEFAULT = "android.intent.extra.ringtone.SHOW_DEFAULT";
    public static final java.lang.String EXTRA_RINGTONE_SHOW_SILENT = "android.intent.extra.ringtone.SHOW_SILENT";
    public static final java.lang.String EXTRA_RINGTONE_TITLE = "android.intent.extra.ringtone.TITLE";
    public static final java.lang.String EXTRA_RINGTONE_TYPE = "android.intent.extra.ringtone.TYPE";
    public static final int ID_COLUMN_INDEX = 0;
    public static final int TITLE_COLUMN_INDEX = 1;
    public static final int TYPE_ALARM = 4;
    public static final int TYPE_ALL = 7;
    public static final int TYPE_NOTIFICATION = 2;
    public static final int TYPE_RINGTONE = 1;
    public static final int URI_COLUMN_INDEX = 2;
    public RingtoneManager(android.app.Activity p0) {}
    public RingtoneManager(android.app.Activity p0, boolean p1) {}
    public RingtoneManager(android.content.Context p0) {}
    public RingtoneManager(android.content.Context p0, boolean p1) {}
    public static void ensureDefaultRingtones(android.content.Context p0) {}
    public static android.net.Uri getActualDefaultRingtoneUri(android.content.Context p0, int p1) { return null; }
    public static android.net.Uri getCacheForType(int p0) { return null; }
    public static android.net.Uri getCacheForType(int p0, int p1) { return null; }
    public static int getDefaultType(android.net.Uri p0) {
        if (p0 == null) return -1;
        if (p0.equals(android.provider.Settings.System.DEFAULT_RINGTONE_URI)) return TYPE_RINGTONE;
        if (p0.equals(android.provider.Settings.System.DEFAULT_NOTIFICATION_URI)) return TYPE_NOTIFICATION;
        if (p0.equals(android.provider.Settings.System.DEFAULT_ALARM_ALERT_URI)) return TYPE_ALARM;
        return -1;
    }
    public static android.net.Uri getDefaultUri(int p0) {
        if ((p0 & TYPE_RINGTONE) != 0) return android.provider.Settings.System.DEFAULT_RINGTONE_URI;
        if ((p0 & TYPE_NOTIFICATION) != 0) return android.provider.Settings.System.DEFAULT_NOTIFICATION_URI;
        if ((p0 & TYPE_ALARM) != 0) return android.provider.Settings.System.DEFAULT_ALARM_ALERT_URI;
        return null;
    }
    public static android.media.Ringtone getRingtone(android.content.Context p0, android.net.Uri p1) { return null; }
    public static android.media.Ringtone getRingtone(android.content.Context p0, android.net.Uri p1, android.media.VolumeShaper.Configuration p2) { return null; }
    public static android.media.Ringtone getRingtone(android.content.Context p0, android.net.Uri p1, android.media.VolumeShaper.Configuration p2, android.media.AudioAttributes p3) { return null; }
    public static android.media.Ringtone getRingtone(android.content.Context p0, android.net.Uri p1, android.media.VolumeShaper.Configuration p2, boolean p3) { return null; }
    public static android.net.Uri getRingtoneUriForRestore(android.content.ContentResolver p0, java.lang.String p1, int p2) { return null; }
    public static android.net.Uri getValidRingtoneUri(android.content.Context p0) { return null; }
    public static boolean hasHapticChannels(android.content.Context p0, android.net.Uri p1) { return false; }
    public static boolean hasHapticChannels(android.net.Uri p0) { return false; }
    public static boolean isDefault(android.net.Uri p0) { return getDefaultType(p0) != -1; }
    public static android.content.res.AssetFileDescriptor openDefaultRingtoneUri(android.content.Context p0, android.net.Uri p1) { return null; }
    public static void setActualDefaultRingtoneUri(android.content.Context p0, int p1, android.net.Uri p2) {}
    public android.net.Uri addCustomExternalRingtone(android.net.Uri p0, int p1) { return null; }
    public android.database.Cursor getCursor() { return (android.database.Cursor) meglerProps.get("Cursor"); }
    public boolean getIncludeDrm() { return (meglerProps.get("IncludeDrm") instanceof Boolean ? (Boolean) meglerProps.get("IncludeDrm") : false); }
    public android.media.Ringtone getRingtone(int p0) { return null; }
    public int getRingtonePosition(android.net.Uri p0) { return 0; }
    public android.net.Uri getRingtoneUri(int p0) { return null; }
    public boolean getStopPreviousRingtone() { return (meglerProps.get("StopPreviousRingtone") instanceof Boolean ? (Boolean) meglerProps.get("StopPreviousRingtone") : false); }
    public boolean hasHapticChannels(int p0) { return false; }
    public int inferStreamType() { return 0; }
    public void setIncludeDrm(boolean p0) { meglerProps.put("IncludeDrm", Boolean.valueOf(p0)); }
    public void setStopPreviousRingtone(boolean p0) { meglerProps.put("StopPreviousRingtone", Boolean.valueOf(p0)); }
    public void setType(int p0) { meglerProps.put("Type", Integer.valueOf(p0)); }
    public void stopPreviousRingtone() {}
    RingtoneManager() { this((android.app.Activity) null); }
}
