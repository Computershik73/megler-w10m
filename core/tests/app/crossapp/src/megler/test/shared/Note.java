package megler.test.shared;

import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

import java.util.ArrayList;

/**
 * A Parcelable both test apps have, each its own copy of this file (crossapp and plugin), as an app and its plug-in each have
 * the classes of their common interface: one app writes it, the other reads it with its own class's CREATOR. It writes
 * numbers and a string, and what a parcel keeps apart from its bytes: a Uri, a list of strings, a Bundle, another Note.
 */
public final class Note implements Parcelable {
    public final int id;
    public final String title;
    public final Uri link;
    public final ArrayList<String> tags = new ArrayList<>();
    public final Bundle more;
    public final Note next;

    public Note(int id, String title, Uri link, Bundle more, Note next, String... tags) {
        this.id = id;
        this.title = title;
        this.link = link;
        this.more = more;
        this.next = next;
        for (String t : tags) this.tags.add(t);
    }

    private Note(Parcel p) {
        id = p.readInt();
        title = p.readString();
        link = p.readParcelable(Note.class.getClassLoader());
        p.readStringList(tags);
        more = p.readBundle(Note.class.getClassLoader());
        next = p.readInt() != 0 ? CREATOR.createFromParcel(p) : null;
    }

    @Override
    public int describeContents() { return 0; }

    @Override
    public void writeToParcel(Parcel p, int flags) {
        p.writeInt(id);
        p.writeString(title);
        p.writeParcelable(link, flags);
        p.writeStringList(tags);
        p.writeBundle(more);
        p.writeInt(next != null ? 1 : 0);
        if (next != null) next.writeToParcel(p, flags);
    }

    public static final Creator<Note> CREATOR = new Creator<Note>() {
        @Override
        public Note createFromParcel(Parcel p) { return new Note(p); }

        @Override
        public Note[] newArray(int n) { return new Note[n]; }
    };

    @Override
    public String toString() {
        return "Note(" + id + ", " + title + ", " + link + ", " + tags + ", " + (more == null ? null : more.getString("k")) + (next == null ? "" : ", " + next) + ")";
    }
}
