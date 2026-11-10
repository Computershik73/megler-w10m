// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// GeocoderParams, as Android 12 and later have it: who asks a geocode provider (the app's uid, package and attribution tag)
// and in which locale, handed with each request of the app's Geocoder to the provider. Google Play services' provider
// (GmsCore's GeocodeChimeraService, a com.android.location.provider.GeocodeProvider) makes the identity of the request of
// those (getClientPackage, getClientUid, getClientAttributionTag) and answers in getLocale's language; microG's likewise.
package android.location;

import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Locale;

/** Who a geocode request is for, and in which language (hidden API: Geocoder makes it, a geocode provider reads it). */
public class GeocoderParams implements Parcelable {
    private final int mUid;
    private final String mPackageName;
    private final String mAttributionTag;
    private final Locale mLocale;

    public GeocoderParams(Context context) { this(context, Locale.getDefault()); }
    public GeocoderParams(Context context, Locale locale) {
        this(android.os.Process.myUid(), context.getPackageName(), context.getAttributionTag(), locale);
    }
    public GeocoderParams(int uid, String packageName, String attributionTag, Locale locale) {
        mUid = uid;
        mPackageName = packageName;
        mAttributionTag = attributionTag;
        mLocale = locale == null ? Locale.getDefault() : locale;
    }

    public Locale getLocale() { return mLocale; }
    public int getClientUid() { return mUid; }
    public String getClientPackage() { return mPackageName; }
    public String getClientAttributionTag() { return mAttributionTag; }

    @Override public String toString() { return "GeocoderParams[uid=" + mUid + " package=" + mPackageName + " tag=" + mAttributionTag + " locale=" + mLocale + "]"; }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel parcel, int flags) {
        parcel.writeInt(mUid);
        parcel.writeString(mPackageName);
        parcel.writeString(mAttributionTag);
        parcel.writeString(mLocale.getLanguage());
        parcel.writeString(mLocale.getCountry());
        parcel.writeString(mLocale.getVariant());
    }
    public static final Parcelable.Creator<GeocoderParams> CREATOR = new Parcelable.Creator<GeocoderParams>() {
        @Override public GeocoderParams createFromParcel(Parcel in) {
            int uid = in.readInt();
            String pkg = in.readString(), tag = in.readString();
            return new GeocoderParams(uid, pkg, tag, new Locale(in.readString(), in.readString(), in.readString()));
        }
        @Override public GeocoderParams[] newArray(int size) { return new GeocoderParams[size]; }
    };
}
