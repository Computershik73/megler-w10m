package android.app;

import android.media.AudioAttributes;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;
import java.util.Arrays;
import java.util.Objects;

/*
 * Megler: Android's (11 to 14). A channel keeps what an app sets on it -- its sound and the sound's audio attributes, its
 * lights, its vibration, its badge, its lock screen visibility, Do Not Disturb, bubbles, whether the user may block it, the
 * conversation it is for -- and has Android's defaults where nothing was set: the default notification sound
 * (Settings.System.DEFAULT_NOTIFICATION_URI) played as Notification.AUDIO_ATTRIBUTES_DEFAULT, no lights (colour 0), no
 * vibration (no pattern), a badge, NotificationManager.VISIBILITY_NO_OVERRIDE, bubbles as the user has them
 * (DEFAULT_ALLOW_BUBBLE). setVibrationPattern turns the vibration on where the pattern has something in it.
 *
 * It was a stub whose setters kept nothing and whose getters answered null, 0 and false. Messenger copies a channel of its
 * own through the getters and asserts that the audio attributes it gave it are there (X.30q.A00 of the version a Lumia 650
 * had under 0.1.0.685: "NullPointerException: INVOKE_RETURN must not be null"), and ended some fifteen seconds after each
 * start, when it made its channels again.
 *
 * copy() is a copy; equals() and hashCode() compare what Android's compare (apps compare the channel the system has with
 * the one they want); a channel goes through a Parcel in Android's layout (writeToParcel, CREATOR); and writeXml and
 * populateFromXml are Android's, for NotificationManager to keep the app's channels across its runs as Android's system
 * keeps them. The members of Android 15 and later (a VibrationEffect, the bundle types) are still the generated ones.
 */
public final class NotificationChannel implements Parcelable {
    public static final String DEFAULT_CHANNEL_ID = "miscellaneous";

    private static final String TAG_CHANNEL = "channel";
    private static final String ATT_NAME = "name";
    private static final String ATT_DESC = "desc";
    private static final String ATT_ID = "id";
    private static final String ATT_DELETED = "deleted";
    private static final String ATT_PRIORITY = "priority";
    private static final String ATT_VISIBILITY = "visibility";
    private static final String ATT_IMPORTANCE = "importance";
    private static final String ATT_LIGHTS = "lights";
    private static final String ATT_LIGHT_COLOR = "light_color";
    private static final String ATT_VIBRATION = "vibration";
    private static final String ATT_VIBRATION_ENABLED = "vibration_enabled";
    private static final String ATT_SOUND = "sound";
    private static final String ATT_USAGE = "usage";
    private static final String ATT_FLAGS = "flags";
    private static final String ATT_CONTENT_TYPE = "content_type";
    private static final String ATT_SHOW_BADGE = "show_badge";
    private static final String ATT_USER_LOCKED = "locked";
    private static final String ATT_FG_SERVICE_SHOWN = "fgservice";
    private static final String ATT_GROUP = "group";
    private static final String ATT_BLOCKABLE_SYSTEM = "blockable_system";
    private static final String ATT_ALLOW_BUBBLE = "allow_bubbles";
    private static final String ATT_ORIG_IMP = "orig_imp";
    private static final String ATT_PARENT_CHANNEL = "parent";
    private static final String ATT_CONVERSATION_ID = "conv_id";
    private static final String ATT_IMP_CONVERSATION = "imp_conv";
    private static final String ATT_DEMOTE = "dem";
    private static final String ATT_DELETED_TIME_MS = "del_time";
    private static final String DELIMITER = ",";

    private static final int DEFAULT_LIGHT_COLOR = 0;
    private static final int DEFAULT_VISIBILITY = NotificationManager.VISIBILITY_NO_OVERRIDE;
    private static final int DEFAULT_IMPORTANCE = NotificationManager.IMPORTANCE_UNSPECIFIED;
    private static final boolean DEFAULT_DELETED = false;
    private static final boolean DEFAULT_SHOW_BADGE = true;
    private static final long DEFAULT_DELETION_TIME_MS = -1;

    private String mId;
    private String mName;
    private String mDesc;
    private int mImportance = DEFAULT_IMPORTANCE;
    private int mOriginalImportance = DEFAULT_IMPORTANCE;
    private boolean mBypassDnd;
    private int mLockscreenVisibility = DEFAULT_VISIBILITY;
    private Uri mSound = android.provider.Settings.System.DEFAULT_NOTIFICATION_URI;
    private boolean mLights;
    private int mLightColor = DEFAULT_LIGHT_COLOR;
    private long[] mVibration;
    // The fields the user changed, which the app may not change afterwards (USER_LOCKED_*).
    private int mUserLockedFields;
    private boolean mUserVisibleTaskShown;
    private boolean mVibrationEnabled;
    private boolean mShowBadge = DEFAULT_SHOW_BADGE;
    private boolean mDeleted = DEFAULT_DELETED;
    private String mGroup;
    private AudioAttributes mAudioAttributes = Notification.AUDIO_ATTRIBUTES_DEFAULT;
    // Whether this is a channel the user may block although the app's notifications are fixed on.
    private boolean mBlockableSystem;
    private int mAllowBubbles = DEFAULT_ALLOW_BUBBLE;
    private boolean mImportanceLockedDefaultApp;
    private String mParentId;
    private String mConversationId;
    private boolean mDemoted;
    private boolean mImportantConvo;
    private long mDeletedTime = DEFAULT_DELETION_TIME_MS;
    private long mLastNotificationUpdateTimeMs;

    public NotificationChannel(String id, CharSequence name, int importance) {
        this.mId = getTrimmedString(id);
        this.mName = name != null ? getTrimmedString(name.toString()) : null;
        this.mImportance = importance;
    }

    protected NotificationChannel(Parcel in) {
        mId = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        mName = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        mDesc = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        mImportance = in.readInt();
        mBypassDnd = in.readByte() != 0;
        mLockscreenVisibility = in.readInt();
        if (in.readByte() != 0) {
            mSound = Uri.CREATOR.createFromParcel(in);
            mSound = Uri.parse(getTrimmedString(mSound.toString()));
        } else {
            mSound = null;
        }
        mLights = in.readByte() != 0;
        mVibration = in.createLongArray();
        if (mVibration != null && mVibration.length > MAX_VIBRATION_LENGTH) mVibration = Arrays.copyOf(mVibration, MAX_VIBRATION_LENGTH);
        mUserLockedFields = in.readInt();
        mUserVisibleTaskShown = in.readByte() != 0;
        mVibrationEnabled = in.readByte() != 0;
        mShowBadge = in.readByte() != 0;
        mDeleted = in.readByte() != 0;
        mGroup = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        mAudioAttributes = in.readInt() > 0 ? AudioAttributes.CREATOR.createFromParcel(in) : null;
        mLightColor = in.readInt();
        mBlockableSystem = in.readBoolean();
        mAllowBubbles = in.readInt();
        mOriginalImportance = in.readInt();
        mParentId = getTrimmedString(in.readString());
        mConversationId = getTrimmedString(in.readString());
        mDemoted = in.readBoolean();
        mImportantConvo = in.readBoolean();
        mDeletedTime = in.readLong();
        mImportanceLockedDefaultApp = in.readBoolean();
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        writeOptional(dest, mId);
        writeOptional(dest, mName);
        writeOptional(dest, mDesc);
        dest.writeInt(mImportance);
        dest.writeByte(mBypassDnd ? (byte) 1 : (byte) 0);
        dest.writeInt(mLockscreenVisibility);
        if (mSound != null) {
            dest.writeByte((byte) 1);
            mSound.writeToParcel(dest, 0);
        } else {
            dest.writeByte((byte) 0);
        }
        dest.writeByte(mLights ? (byte) 1 : (byte) 0);
        dest.writeLongArray(mVibration);
        dest.writeInt(mUserLockedFields);
        dest.writeByte(mUserVisibleTaskShown ? (byte) 1 : (byte) 0);
        dest.writeByte(mVibrationEnabled ? (byte) 1 : (byte) 0);
        dest.writeByte(mShowBadge ? (byte) 1 : (byte) 0);
        dest.writeByte(mDeleted ? (byte) 1 : (byte) 0);
        writeOptional(dest, mGroup);
        if (mAudioAttributes != null) {
            dest.writeInt(1);
            mAudioAttributes.writeToParcel(dest, 0);
        } else {
            dest.writeInt(0);
        }
        dest.writeInt(mLightColor);
        dest.writeBoolean(mBlockableSystem);
        dest.writeInt(mAllowBubbles);
        dest.writeInt(mOriginalImportance);
        dest.writeString(mParentId);
        dest.writeString(mConversationId);
        dest.writeBoolean(mDemoted);
        dest.writeBoolean(mImportantConvo);
        dest.writeLong(mDeletedTime);
        dest.writeBoolean(mImportanceLockedDefaultApp);
    }

    /** A string after a byte that says whether there is one, as Android's writes the channel's optional texts. */
    private static void writeOptional(Parcel dest, String s) {
        if (s != null) {
            dest.writeByte((byte) 1);
            dest.writeString(s);
        } else {
            dest.writeByte((byte) 0);
        }
    }

    public NotificationChannel copy() {
        NotificationChannel copy = new NotificationChannel(mId, mName, mImportance);
        copy.setDescription(mDesc);
        copy.setBypassDnd(mBypassDnd);
        copy.setLockscreenVisibility(mLockscreenVisibility);
        copy.setSound(mSound, mAudioAttributes);
        copy.setLightColor(mLightColor);
        copy.enableLights(mLights);
        copy.setVibrationPattern(mVibration);
        copy.lockFields(mUserLockedFields);
        copy.setUserVisibleTaskShown(mUserVisibleTaskShown);
        copy.enableVibration(mVibrationEnabled);
        copy.setShowBadge(mShowBadge);
        copy.setDeleted(mDeleted);
        copy.setGroup(mGroup);
        copy.setBlockable(mBlockableSystem);
        copy.setAllowBubbles(mAllowBubbles);
        copy.setOriginalImportance(mOriginalImportance);
        copy.setConversationId(mParentId, mConversationId);
        copy.setDemoted(mDemoted);
        copy.setImportantConversation(mImportantConvo);
        copy.setDeletedTimeMs(mDeletedTime);
        copy.setImportanceLockedByCriticalDeviceFunction(mImportanceLockedDefaultApp);
        copy.setLastNotificationUpdateTimeMs(mLastNotificationUpdateTimeMs);
        return copy;
    }

    public void lockFields(int field) { mUserLockedFields |= field; }
    public void unlockFields(int field) { mUserLockedFields &= ~field; }
    public void setUserVisibleTaskShown(boolean shown) { mUserVisibleTaskShown = shown; }
    public void setDeleted(boolean deleted) { mDeleted = deleted; }
    public void setDeletedTimeMs(long time) { mDeletedTime = time; }
    public void setImportantConversation(boolean importantConvo) { mImportantConvo = importantConvo; }
    public void setBlockable(boolean blockable) { mBlockableSystem = blockable; }

    // Modifiable by apps after the channel is made.
    public void setName(CharSequence name) { mName = name != null ? getTrimmedString(name.toString()) : null; }
    public void setDescription(String description) { mDesc = getTrimmedString(description); }

    private String getTrimmedString(String input) {
        if (input != null && input.length() > MAX_TEXT_LENGTH) return input.substring(0, MAX_TEXT_LENGTH);
        return input;
    }

    public void setId(String id) { mId = id; }

    // Modifiable by apps before the channel is made (NotificationManager.createNotificationChannel).
    public void setGroup(String groupId) { this.mGroup = groupId; }
    public void setShowBadge(boolean showBadge) { this.mShowBadge = showBadge; }
    public void setSound(Uri sound, AudioAttributes audioAttributes) {
        this.mSound = sound;
        this.mAudioAttributes = audioAttributes;
    }
    public void enableLights(boolean lights) { this.mLights = lights; }
    public void setLightColor(int argb) { this.mLightColor = argb; }
    public void enableVibration(boolean vibration) { this.mVibrationEnabled = vibration; }
    /** The pattern, and the vibration on where it has something in it, off where it has nothing (Android's). */
    public void setVibrationPattern(long[] vibrationPattern) {
        this.mVibrationEnabled = vibrationPattern != null && vibrationPattern.length > 0;
        this.mVibration = vibrationPattern;
    }
    public void setImportance(int importance) { this.mImportance = importance; }

    // Modifiable by the system and a notification ranker (NotificationManager sets them back for an app's new channel).
    public void setBypassDnd(boolean bypassDnd) { this.mBypassDnd = bypassDnd; }
    public void setLockscreenVisibility(int lockscreenVisibility) { this.mLockscreenVisibility = lockscreenVisibility; }

    /** Not respected since Android 11 (canBubble is the user's); NotificationManager sets it back for an app's new channel. */
    public void setAllowBubbles(boolean allowBubbles) { mAllowBubbles = allowBubbles ? ALLOW_BUBBLE_ON : ALLOW_BUBBLE_OFF; }
    public void setAllowBubbles(int allowed) { mAllowBubbles = allowed; }

    public void setConversationId(String parentChannelId, String conversationId) {
        mParentId = parentChannelId;
        mConversationId = conversationId;
    }

    public String getId() { return mId; }
    public CharSequence getName() { return mName; }
    public String getDescription() { return mDesc; }
    public int getImportance() { return mImportance; }
    public boolean canBypassDnd() { return mBypassDnd; }
    public boolean isConversation() { return !TextUtils.isEmpty(getConversationId()); }
    public boolean isImportantConversation() { return mImportantConvo; }
    public Uri getSound() { return mSound; }
    public AudioAttributes getAudioAttributes() { return mAudioAttributes; }
    public boolean shouldShowLights() { return mLights; }
    public int getLightColor() { return mLightColor; }
    public boolean shouldVibrate() { return mVibrationEnabled; }
    public long[] getVibrationPattern() { return mVibration; }
    public int getLockscreenVisibility() { return mLockscreenVisibility; }
    public boolean canShowBadge() { return mShowBadge; }
    public String getGroup() { return mGroup; }
    public boolean canBubble() { return mAllowBubbles == ALLOW_BUBBLE_ON; }
    public int getAllowBubbles() { return mAllowBubbles; }
    public String getParentChannelId() { return mParentId; }
    public String getConversationId() { return mConversationId; }
    public boolean isDeleted() { return mDeleted; }
    public long getDeletedTimeMs() { return mDeletedTime; }
    public int getUserLockedFields() { return mUserLockedFields; }
    public boolean isUserVisibleTaskShown() { return mUserVisibleTaskShown; }
    public boolean isBlockable() { return mBlockableSystem; }
    public void setImportanceLockedByCriticalDeviceFunction(boolean locked) { mImportanceLockedDefaultApp = locked; }
    public boolean isImportanceLockedByCriticalDeviceFunction() { return mImportanceLockedDefaultApp; }
    public int getOriginalImportance() { return mOriginalImportance; }
    public void setOriginalImportance(int importance) { mOriginalImportance = importance; }
    public void setDemoted(boolean demoted) { mDemoted = demoted; }
    public boolean isDemoted() { return mDemoted; }
    public boolean hasUserSetImportance() { return (mUserLockedFields & USER_LOCKED_IMPORTANCE) != 0; }
    public boolean hasUserSetSound() { return (mUserLockedFields & USER_LOCKED_SOUND) != 0; }
    public long getLastNotificationUpdateTimeMs() { return mLastNotificationUpdateTimeMs; }
    public void setLastNotificationUpdateTimeMs(long updateTimeMs) { mLastNotificationUpdateTimeMs = updateTimeMs; }

    /**
     * What writeXml wrote, as Android's system reads a channel back (its name, id and importance are the constructor's): a
     * sound with Android's audio attributes for a notification where the element has none.
     */
    public void populateFromXml(org.xmlpull.v1.XmlPullParser parser) {
        setDescription(parser.getAttributeValue(null, ATT_DESC));
        setBypassDnd(Notification.PRIORITY_DEFAULT != safeInt(parser, ATT_PRIORITY, Notification.PRIORITY_DEFAULT));
        setLockscreenVisibility(safeInt(parser, ATT_VISIBILITY, DEFAULT_VISIBILITY));
        setSound(safeUri(parser, ATT_SOUND), safeAudioAttributes(parser));
        enableLights(safeBool(parser, ATT_LIGHTS, false));
        setLightColor(safeInt(parser, ATT_LIGHT_COLOR, DEFAULT_LIGHT_COLOR));
        setVibrationPattern(safeLongArray(parser, ATT_VIBRATION, null));
        enableVibration(safeBool(parser, ATT_VIBRATION_ENABLED, false));
        setShowBadge(safeBool(parser, ATT_SHOW_BADGE, false));
        setDeleted(safeBool(parser, ATT_DELETED, false));
        setDeletedTimeMs(safeLong(parser, ATT_DELETED_TIME_MS, DEFAULT_DELETION_TIME_MS));
        setGroup(parser.getAttributeValue(null, ATT_GROUP));
        lockFields(safeInt(parser, ATT_USER_LOCKED, 0));
        setUserVisibleTaskShown(safeBool(parser, ATT_FG_SERVICE_SHOWN, false));
        setBlockable(safeBool(parser, ATT_BLOCKABLE_SYSTEM, false));
        setAllowBubbles(safeInt(parser, ATT_ALLOW_BUBBLE, DEFAULT_ALLOW_BUBBLE));
        setOriginalImportance(safeInt(parser, ATT_ORIG_IMP, DEFAULT_IMPORTANCE));
        setConversationId(parser.getAttributeValue(null, ATT_PARENT_CHANNEL), parser.getAttributeValue(null, ATT_CONVERSATION_ID));
        setDemoted(safeBool(parser, ATT_DEMOTE, false));
        setImportantConversation(safeBool(parser, ATT_IMP_CONVERSATION, false));
    }

    /** The channel as an element of Android's notification policy file (its attributes: what differs from the defaults). */
    public void writeXml(org.xmlpull.v1.XmlSerializer out) throws java.io.IOException {
        out.startTag(null, TAG_CHANNEL);
        attribute(out, ATT_ID, getId());
        if (getName() != null) attribute(out, ATT_NAME, getName().toString());
        if (getDescription() != null) attribute(out, ATT_DESC, getDescription());
        if (getImportance() != DEFAULT_IMPORTANCE) attribute(out, ATT_IMPORTANCE, Integer.toString(getImportance()));
        if (canBypassDnd()) attribute(out, ATT_PRIORITY, Integer.toString(Notification.PRIORITY_MAX));
        if (getLockscreenVisibility() != DEFAULT_VISIBILITY) attribute(out, ATT_VISIBILITY, Integer.toString(getLockscreenVisibility()));
        if (getSound() != null) attribute(out, ATT_SOUND, getSound().toString());
        if (getAudioAttributes() != null) {
            attribute(out, ATT_USAGE, Integer.toString(getAudioAttributes().getUsage()));
            attribute(out, ATT_CONTENT_TYPE, Integer.toString(getAudioAttributes().getContentType()));
            attribute(out, ATT_FLAGS, Integer.toString(getAudioAttributes().getFlags()));
        }
        if (shouldShowLights()) attribute(out, ATT_LIGHTS, Boolean.toString(shouldShowLights()));
        if (getLightColor() != DEFAULT_LIGHT_COLOR) attribute(out, ATT_LIGHT_COLOR, Integer.toString(getLightColor()));
        if (shouldVibrate()) attribute(out, ATT_VIBRATION_ENABLED, Boolean.toString(shouldVibrate()));
        if (getVibrationPattern() != null) attribute(out, ATT_VIBRATION, longArrayToString(getVibrationPattern()));
        if (getUserLockedFields() != 0) attribute(out, ATT_USER_LOCKED, Integer.toString(getUserLockedFields()));
        if (isUserVisibleTaskShown()) attribute(out, ATT_FG_SERVICE_SHOWN, Boolean.toString(isUserVisibleTaskShown()));
        if (canShowBadge()) attribute(out, ATT_SHOW_BADGE, Boolean.toString(canShowBadge()));
        if (isDeleted()) attribute(out, ATT_DELETED, Boolean.toString(isDeleted()));
        if (getDeletedTimeMs() >= 0) attribute(out, ATT_DELETED_TIME_MS, Long.toString(getDeletedTimeMs()));
        if (getGroup() != null) attribute(out, ATT_GROUP, getGroup());
        if (isBlockable()) attribute(out, ATT_BLOCKABLE_SYSTEM, Boolean.toString(isBlockable()));
        if (getAllowBubbles() != DEFAULT_ALLOW_BUBBLE) attribute(out, ATT_ALLOW_BUBBLE, Integer.toString(getAllowBubbles()));
        if (getOriginalImportance() != DEFAULT_IMPORTANCE) attribute(out, ATT_ORIG_IMP, Integer.toString(getOriginalImportance()));
        if (getParentChannelId() != null) attribute(out, ATT_PARENT_CHANNEL, getParentChannelId());
        if (getConversationId() != null) attribute(out, ATT_CONVERSATION_ID, getConversationId());
        if (isDemoted()) attribute(out, ATT_DEMOTE, Boolean.toString(isDemoted()));
        if (isImportantConversation()) attribute(out, ATT_IMP_CONVERSATION, Boolean.toString(isImportantConversation()));
        // (mImportanceLockedDefaultApp is the system's to work out each time; Android writes it nowhere either.)
        out.endTag(null, TAG_CHANNEL);
    }

    /**
     * An attribute whose characters XML can hold: one it cannot (a control character, half a surrogate pair) is written as
     * U+FFFD, where kXML's serializer would refuse the whole file. None for no value.
     */
    static void attribute(org.xmlpull.v1.XmlSerializer out, String name, String value) throws java.io.IOException {
        if (value == null) return;
        StringBuilder safe = null;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            boolean ok = c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd);
            if (!ok && Character.isHighSurrogate(c) && i + 1 < value.length() && Character.isLowSurrogate(value.charAt(i + 1))) {
                if (safe != null) safe.append(c).append(value.charAt(i + 1));
                i++;
                continue;
            }
            if (!ok && safe == null) safe = new StringBuilder(value.substring(0, i));
            if (safe != null) safe.append(ok ? c : '�');
        }
        out.attribute(null, name, safe != null ? safe.toString() : value);
    }

    private static Uri safeUri(org.xmlpull.v1.XmlPullParser parser, String att) {
        final String val = parser.getAttributeValue(null, att);
        return val == null ? null : Uri.parse(val);
    }

    private static AudioAttributes safeAudioAttributes(org.xmlpull.v1.XmlPullParser parser) {
        int usage = safeInt(parser, ATT_USAGE, AudioAttributes.USAGE_NOTIFICATION);
        int contentType = safeInt(parser, ATT_CONTENT_TYPE, AudioAttributes.CONTENT_TYPE_SONIFICATION);
        int flags = safeInt(parser, ATT_FLAGS, 0);
        return new AudioAttributes.Builder().setUsage(usage).setContentType(contentType).setFlags(flags).build();
    }

    private static int safeInt(org.xmlpull.v1.XmlPullParser parser, String att, int defValue) {
        final String value = parser.getAttributeValue(null, att);
        if (TextUtils.isEmpty(value)) return defValue;
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException e) {
            return defValue;
        }
    }

    private static long safeLong(org.xmlpull.v1.XmlPullParser parser, String att, long defValue) {
        final String value = parser.getAttributeValue(null, att);
        if (TextUtils.isEmpty(value)) return defValue;
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException e) {
            return defValue;
        }
    }

    private static boolean safeBool(org.xmlpull.v1.XmlPullParser parser, String att, boolean defValue) {
        final String value = parser.getAttributeValue(null, att);
        if (TextUtils.isEmpty(value)) return defValue;
        return Boolean.parseBoolean(value);
    }

    private static long[] safeLongArray(org.xmlpull.v1.XmlPullParser parser, String att, long[] defValue) {
        final String attributeValue = parser.getAttributeValue(null, att);
        if (TextUtils.isEmpty(attributeValue)) return defValue;
        String[] values = attributeValue.split(DELIMITER);
        long[] longValues = new long[values.length];
        for (int i = 0; i < values.length; i++) {
            try {
                longValues[i] = Long.parseLong(values[i]);
            } catch (NumberFormatException e) {
                longValues[i] = 0;
            }
        }
        return longValues;
    }

    private static String longArrayToString(long[] values) {
        StringBuilder sb = new StringBuilder();
        if (values != null && values.length > 0) {
            for (int i = 0; i < values.length - 1; i++) sb.append(values[i]).append(DELIMITER);
            sb.append(values[values.length - 1]);
        }
        return sb.toString();
    }

    public static final Parcelable.Creator<NotificationChannel> CREATOR = new Parcelable.Creator<NotificationChannel>() {
        @Override public NotificationChannel createFromParcel(Parcel in) { return new NotificationChannel(in); }
        @Override public NotificationChannel[] newArray(int size) { return new NotificationChannel[size]; }
    };

    public int describeContents() { return 0; }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        NotificationChannel that = (NotificationChannel) o;
        return getImportance() == that.getImportance()
                && mBypassDnd == that.mBypassDnd
                && getLockscreenVisibility() == that.getLockscreenVisibility()
                && mLights == that.mLights
                && getLightColor() == that.getLightColor()
                && getUserLockedFields() == that.getUserLockedFields()
                && isUserVisibleTaskShown() == that.isUserVisibleTaskShown()
                && mVibrationEnabled == that.mVibrationEnabled
                && mShowBadge == that.mShowBadge
                && isDeleted() == that.isDeleted()
                && getDeletedTimeMs() == that.getDeletedTimeMs()
                && isBlockable() == that.isBlockable()
                && mAllowBubbles == that.mAllowBubbles
                && Objects.equals(getId(), that.getId())
                && Objects.equals(getName(), that.getName())
                && Objects.equals(mDesc, that.mDesc)
                && Objects.equals(getSound(), that.getSound())
                && Arrays.equals(mVibration, that.mVibration)
                && Objects.equals(getGroup(), that.getGroup())
                && Objects.equals(getAudioAttributes(), that.getAudioAttributes())
                && mImportanceLockedDefaultApp == that.mImportanceLockedDefaultApp
                && mOriginalImportance == that.mOriginalImportance
                && Objects.equals(getParentChannelId(), that.getParentChannelId())
                && Objects.equals(getConversationId(), that.getConversationId())
                && isDemoted() == that.isDemoted()
                && isImportantConversation() == that.isImportantConversation();
    }

    @Override
    public int hashCode() {
        int result = Objects.hash(getId(), getName(), mDesc, getImportance(), mBypassDnd, getLockscreenVisibility(), getSound(), mLights,
                getLightColor(), getUserLockedFields(), isUserVisibleTaskShown(), mVibrationEnabled, mShowBadge, isDeleted(), getGroup(),
                getAudioAttributes(), isBlockable(), mAllowBubbles, mImportanceLockedDefaultApp, mOriginalImportance, mParentId,
                mConversationId, mDemoted, mImportantConvo);
        result = 31 * result + Arrays.hashCode(mVibration);
        return result;
    }

    @Override
    public String toString() {
        return "NotificationChannel{"
                + "mId='" + mId + '\''
                + ", mName=" + mName
                + ", mDescription=" + (!TextUtils.isEmpty(mDesc) ? "hasDescription " : "")
                + ", mImportance=" + mImportance
                + ", mBypassDnd=" + mBypassDnd
                + ", mLockscreenVisibility=" + mLockscreenVisibility
                + ", mSound=" + mSound
                + ", mLights=" + mLights
                + ", mLightColor=" + mLightColor
                + ", mVibration=" + Arrays.toString(mVibration)
                + ", mUserLockedFields=" + Integer.toHexString(mUserLockedFields)
                + ", mUserVisibleTaskShown=" + mUserVisibleTaskShown
                + ", mVibrationEnabled=" + mVibrationEnabled
                + ", mShowBadge=" + mShowBadge
                + ", mDeleted=" + mDeleted
                + ", mDeletedTimeMs=" + mDeletedTime
                + ", mGroup='" + mGroup + '\''
                + ", mAudioAttributes=" + mAudioAttributes
                + ", mBlockableSystem=" + mBlockableSystem
                + ", mAllowBubbles=" + mAllowBubbles
                + ", mImportanceLockedDefaultApp=" + mImportanceLockedDefaultApp
                + ", mOriginalImp=" + mOriginalImportance
                + ", mParent=" + mParentId
                + ", mConversationId=" + mConversationId
                + ", mDemoted=" + mDemoted
                + ", mImportantConvo=" + mImportantConvo
                + ", mLastNotificationUpdateTimeMs=" + mLastNotificationUpdateTimeMs
                + '}';
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    public static final int ALLOW_BUBBLE_OFF = 0;
    public static final int ALLOW_BUBBLE_ON = 1;
    public static final int DEFAULT_ALLOW_BUBBLE = -1;
    public static final java.lang.String EDIT_CONVERSATION = "conversation";
    public static final java.lang.String EDIT_IMPORTANCE = "importance";
    public static final java.lang.String EDIT_LAUNCHER = "launcher";
    public static final java.lang.String EDIT_LOCKED_DEVICE = "locked";
    public static final java.lang.String EDIT_SOUND = "sound";
    public static final java.lang.String EDIT_VIBRATION = "vibration";
    public static final java.lang.String EDIT_ZEN = "zen";
    public static int[] LOCKABLE_FIELDS;
    public static final int MAX_SERIALIZED_VIBRATION_LENGTH = 32768;
    public static final int MAX_TEXT_LENGTH = 1000;
    public static final int MAX_VIBRATION_LENGTH = 500;
    public static final java.lang.String NEWS_ID = "android.app.news";
    public static final java.lang.String OLD_CONVERSATION_CHANNEL_ID_FORMAT = "%1$s : %2$s";
    public static final java.lang.String PLACEHOLDER_CONVERSATION_ID = ":placeholder_id";
    public static final java.lang.String PROMOTIONS_ID = "android.app.promotions";
    public static final java.lang.String RECS_ID = "android.app.recs";
    public static final java.lang.String SOCIAL_MEDIA_ID = "android.app.social";
    public static java.util.ArrayList SYSTEM_RESERVED_IDS;
    public static final int USER_LOCKED_ALLOW_BUBBLE = 256;
    public static final int USER_LOCKED_IMPORTANCE = 4;
    public static final int USER_LOCKED_LIGHTS = 8;
    public static final int USER_LOCKED_PRIORITY = 1;
    public static final int USER_LOCKED_SHOW_BADGE = 128;
    public static final int USER_LOCKED_SOUND = 32;
    public static final int USER_LOCKED_VIBRATION = 16;
    public static final int USER_LOCKED_VISIBILITY = 2;
    public static java.lang.String getChannelIdForBundleType(int p0) { return null; }
    public void dump(java.io.PrintWriter p0, java.lang.String p1, boolean p2) {}
    public void dumpDebug(android.util.proto.ProtoOutputStream p0, long p1) {}
    public android.os.VibrationEffect getVibrationEffect() { return (android.os.VibrationEffect) megler.Fill.get(meglerFill, "VibrationEffect"); }
    public boolean isSoundRestored() { return false; }
    public void populateFromXmlForRestore(org.xmlpull.v1.XmlPullParser p0, boolean p1, android.content.Context p2) {}
    public android.net.Uri restoreSoundUri(android.content.Context p0, android.net.Uri p1, boolean p2, int p3) { return null; }
    public void setVibrationEffect(android.os.VibrationEffect p0) { meglerFill = megler.Fill.put(meglerFill, "VibrationEffect", p0); }
    public org.json.JSONObject toJson() { return null; }
    public void writeXmlForBackup(org.xmlpull.v1.XmlSerializer p0, android.content.Context p1) {}
    // ---- end of generated members
}
