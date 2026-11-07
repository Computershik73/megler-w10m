package android.content;

import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** What one operation of a batch gave, as Android's: an insert's URI, a count of rows, a call's extras, or what it threw. */
public class ContentProviderResult implements Parcelable {
    public final Uri uri;
    public final Integer count;
    public final Bundle extras;
    public final Throwable exception;

    public ContentProviderResult(Uri uri) { this(java.util.Objects.requireNonNull(uri), null, null, null); }
    public ContentProviderResult(int count) { this(null, count, null, null); }
    public ContentProviderResult(Bundle extras) { this(null, null, java.util.Objects.requireNonNull(extras), null); }
    public ContentProviderResult(Throwable exception) { this(null, null, null, exception); }
    public ContentProviderResult(Uri uri, Integer count, Bundle extras, Throwable exception) {
        this.uri = uri;
        this.count = count;
        this.extras = extras;
        this.exception = exception;
    }
    /** Android's, for another user; there is one here. */
    public ContentProviderResult(ContentProviderResult r, int userId) { this(r.uri, r.count, r.extras, r.exception); }
    // In this one process a Parcel carries objects as they are: the result itself goes through it.
    public ContentProviderResult(Parcel p) { this(read(p), 0); }
    private static ContentProviderResult read(Parcel p) { Object o = p.readValue(null); return o instanceof ContentProviderResult ? (ContentProviderResult) o : new ContentProviderResult(); }
    public ContentProviderResult() { this(null, null, null, null); }
    @Override public String toString() {
        StringBuilder s = new StringBuilder("ContentProviderResult(");
        if (uri != null) s.append("uri=").append(uri).append(' ');
        if (count != null) s.append("count=").append(count).append(' ');
        if (extras != null) s.append("extras=").append(extras).append(' ');
        if (exception != null) s.append("exception=").append(exception).append(' ');
        s.setLength(s.length() - 1);
        return s.append(')').toString();
    }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel p, int flags) { p.writeValue(this); }
    public static final Parcelable.Creator<ContentProviderResult> CREATOR = new Parcelable.Creator<ContentProviderResult>() {
        public ContentProviderResult createFromParcel(Parcel p) { return (ContentProviderResult) p.readValue(null); }
        public ContentProviderResult[] newArray(int n) { return new ContentProviderResult[n]; }
    };
}
