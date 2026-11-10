package android.app;

import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/*
 * Megler: Android's (11 to 14). A group keeps its description, whether it is blocked and the channels in it, and has
 * Android's equals, hashCode, toString and Parcel layout, and its clone(), public API since Android 9, which the stub did
 * not have (an app's call of it found no such method). The stub kept no description and gave a new empty list of channels
 * at each call. NotificationManager hands out each group with the app's channels in it, as Android's system does, and
 * keeps the groups across the app's runs (writeXml, populateFromXml: Android's elements of its notification policy file).
 */
public final class NotificationChannelGroup implements Parcelable {
    private static final String TAG_GROUP = "channelGroup";
    private static final String ATT_NAME = "name";
    private static final String ATT_DESCRIPTION = "desc";
    private static final String ATT_ID = "id";
    private static final String ATT_BLOCKED = "blocked";
    private static final String ATT_USER_LOCKED = "locked";

    private final String mId;
    private CharSequence mName;
    private String mDescription;
    private boolean mBlocked;
    private List<NotificationChannel> mChannels = new ArrayList<>();
    // The fields the user changed (USER_LOCKED_BLOCKED_STATE).
    private int mUserLockedFields;

    public NotificationChannelGroup(String id, CharSequence name) {
        this.mId = getTrimmedString(id);
        this.mName = name != null ? getTrimmedString(name.toString()) : null;
    }

    protected NotificationChannelGroup(Parcel in) {
        mId = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        mName = in.readByte() != 0 ? getTrimmedString(in.readString()) : "";
        mDescription = in.readByte() != 0 ? getTrimmedString(in.readString()) : null;
        in.readParcelableList(mChannels, NotificationChannel.class.getClassLoader());
        mBlocked = in.readBoolean();
        mUserLockedFields = in.readInt();
    }

    private String getTrimmedString(String input) {
        if (input != null && input.length() > MAX_TEXT_LENGTH) return input.substring(0, MAX_TEXT_LENGTH);
        return input;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        if (mId != null) {
            dest.writeByte((byte) 1);
            dest.writeString(mId);
        } else {
            dest.writeByte((byte) 0);
        }
        if (mName != null) {
            dest.writeByte((byte) 1);
            dest.writeString(mName.toString());
        } else {
            dest.writeByte((byte) 0);
        }
        if (mDescription != null) {
            dest.writeByte((byte) 1);
            dest.writeString(mDescription);
        } else {
            dest.writeByte((byte) 0);
        }
        dest.writeParcelableList(mChannels, flags);
        dest.writeBoolean(mBlocked);
        dest.writeInt(mUserLockedFields);
    }

    public String getId() { return mId; }
    public CharSequence getName() { return mName; }
    public String getDescription() { return mDescription; }
    public List<NotificationChannel> getChannels() { return mChannels; }
    public boolean isBlocked() { return mBlocked; }
    public void setDescription(String description) { mDescription = getTrimmedString(description); }
    public void setBlocked(boolean blocked) { mBlocked = blocked; }
    public void addChannel(NotificationChannel channel) { mChannels.add(channel); }
    public void setChannels(List<NotificationChannel> channels) { mChannels = channels; }
    public void lockFields(int field) { mUserLockedFields |= field; }
    public void unlockFields(int field) { mUserLockedFields &= ~field; }
    public int getUserLockedFields() { return mUserLockedFields; }

    /** What writeXml wrote, as Android's system reads a group back (its id and name are the constructor's). */
    public void populateFromXml(org.xmlpull.v1.XmlPullParser parser) {
        setDescription(parser.getAttributeValue(null, ATT_DESCRIPTION));
        String blocked = parser.getAttributeValue(null, ATT_BLOCKED);
        setBlocked(!TextUtils.isEmpty(blocked) && Boolean.parseBoolean(blocked));
    }

    /** The group as an element of Android's notification policy file. */
    public void writeXml(org.xmlpull.v1.XmlSerializer out) throws java.io.IOException {
        out.startTag(null, TAG_GROUP);
        NotificationChannel.attribute(out, ATT_ID, getId());
        if (getName() != null) NotificationChannel.attribute(out, ATT_NAME, getName().toString());
        if (getDescription() != null) NotificationChannel.attribute(out, ATT_DESCRIPTION, getDescription());
        NotificationChannel.attribute(out, ATT_BLOCKED, Boolean.toString(isBlocked()));
        NotificationChannel.attribute(out, ATT_USER_LOCKED, Integer.toString(mUserLockedFields));
        out.endTag(null, TAG_GROUP);
    }

    public static final Parcelable.Creator<NotificationChannelGroup> CREATOR = new Parcelable.Creator<NotificationChannelGroup>() {
        @Override public NotificationChannelGroup createFromParcel(Parcel in) { return new NotificationChannelGroup(in); }
        @Override public NotificationChannelGroup[] newArray(int size) { return new NotificationChannelGroup[size]; }
    };

    public int describeContents() { return 0; }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        NotificationChannelGroup that = (NotificationChannelGroup) o;
        return isBlocked() == that.isBlocked()
                && mUserLockedFields == that.mUserLockedFields
                && Objects.equals(getId(), that.getId())
                && Objects.equals(getName(), that.getName())
                && Objects.equals(getDescription(), that.getDescription())
                && Objects.equals(getChannels(), that.getChannels());
    }

    @Override
    public int hashCode() { return Objects.hash(getId(), getName(), getDescription(), isBlocked(), getChannels(), mUserLockedFields); }

    /** Android's: a group of the same id, name, description, blocked state and locked fields, with the same list of channels. */
    @Override
    public NotificationChannelGroup clone() {
        NotificationChannelGroup cloned = new NotificationChannelGroup(getId(), getName());
        cloned.setDescription(getDescription());
        cloned.setBlocked(isBlocked());
        cloned.setChannels(getChannels());
        cloned.lockFields(mUserLockedFields);
        return cloned;
    }

    @Override
    public String toString() {
        return "NotificationChannelGroup{"
                + "mId='" + mId + '\''
                + ", mName=" + mName
                + ", mDescription=" + (!TextUtils.isEmpty(mDescription) ? "hasDescription " : "")
                + ", mBlocked=" + mBlocked
                + ", mChannels=" + mChannels
                + ", mUserLockedFields=" + mUserLockedFields
                + '}';
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final int MAX_TEXT_LENGTH = 1000;
    public static final int USER_LOCKED_BLOCKED_STATE = 1;
    public void dumpDebug(android.util.proto.ProtoOutputStream p0, long p1) {}
    public org.json.JSONObject toJson() { return null; }
    // ---- end of generated members
}
