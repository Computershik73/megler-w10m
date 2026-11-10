// Overlay of core/framework/gen-src/android/location/Address.java (tools/build-framework.sh), whose class was signatures that
// kept some fields in a map: an address had no locale and no lines (getAddressLine was always null, setAddressLine did
// nothing), it never had a latitude or longitude (hasLatitude false whatever was set), and it went through no Parcel (no
// CREATOR, writeToParcel wrote nothing). Android's Geocoder gives these to an app (android.location.Geocoder, from the
// geocode provider of Google Play services hosted in the app), and apps read the lines first (Transit's place names).
// Written as Android's: its fields, its numbering of address lines (the largest index set, -1 for none), getLatitude and
// getLongitude throwing when none was set, setExtras keeping a copy, toString, and its
// Parcel layout (the locale's language and country, the lines as index and text, the fields, the coordinates each after a
// flag, the phone, the URL and the extras).
package android.location;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** A place as a set of strings: its address lines, its parts (locality, thoroughfare...), and where it is when that is known. */
public class Address implements Parcelable {
    private Locale mLocale;
    private HashMap<Integer, String> mAddressLines;
    private int mMaxAddressLineIndex = -1;
    private String mFeatureName, mAdminArea, mSubAdminArea, mLocality, mSubLocality, mThoroughfare, mSubThoroughfare, mPremises,
        mPostalCode, mCountryCode, mCountryName, mPhone, mUrl;
    private double mLatitude, mLongitude;
    private boolean mHasLatitude, mHasLongitude;
    private Bundle mExtras;

    public Address(Locale locale) { mLocale = locale; }

    public Locale getLocale() { return mLocale; }

    /** The largest index an address line was set at, -1 when none is. */
    public int getMaxAddressLineIndex() { return mMaxAddressLineIndex; }
    public String getAddressLine(int index) {
        if (index < 0) throw new IllegalArgumentException("index = " + index + " < 0");
        return mAddressLines == null ? null : mAddressLines.get(index);
    }
    /** A line at its index (from 0), which may be null; the largest index is counted again then, over every index set, as Android does. */
    public void setAddressLine(int index, String line) {
        if (index < 0) throw new IllegalArgumentException("index = " + index + " < 0");
        if (mAddressLines == null) mAddressLines = new HashMap<>();
        mAddressLines.put(index, line);
        if (line != null) {
            mMaxAddressLineIndex = Math.max(mMaxAddressLineIndex, index);
            return;
        }
        mMaxAddressLineIndex = -1;
        for (Map.Entry<Integer, String> e : mAddressLines.entrySet()) mMaxAddressLineIndex = Math.max(mMaxAddressLineIndex, e.getKey());
    }

    public String getFeatureName() { return mFeatureName; }
    public void setFeatureName(String featureName) { mFeatureName = featureName; }
    public String getAdminArea() { return mAdminArea; }
    public void setAdminArea(String adminArea) { mAdminArea = adminArea; }
    public String getSubAdminArea() { return mSubAdminArea; }
    public void setSubAdminArea(String subAdminArea) { mSubAdminArea = subAdminArea; }
    public String getLocality() { return mLocality; }
    public void setLocality(String locality) { mLocality = locality; }
    public String getSubLocality() { return mSubLocality; }
    public void setSubLocality(String subLocality) { mSubLocality = subLocality; }
    public String getThoroughfare() { return mThoroughfare; }
    public void setThoroughfare(String thoroughfare) { mThoroughfare = thoroughfare; }
    public String getSubThoroughfare() { return mSubThoroughfare; }
    public void setSubThoroughfare(String subThoroughfare) { mSubThoroughfare = subThoroughfare; }
    public String getPremises() { return mPremises; }
    public void setPremises(String premises) { mPremises = premises; }
    public String getPostalCode() { return mPostalCode; }
    public void setPostalCode(String postalCode) { mPostalCode = postalCode; }
    public String getCountryCode() { return mCountryCode; }
    public void setCountryCode(String countryCode) { mCountryCode = countryCode; }
    public String getCountryName() { return mCountryName; }
    public void setCountryName(String countryName) { mCountryName = countryName; }
    public String getPhone() { return mPhone; }
    public void setPhone(String phone) { mPhone = phone; }
    public String getUrl() { return mUrl; }
    public void setUrl(String url) { mUrl = url; }

    public boolean hasLatitude() { return mHasLatitude; }
    /** The latitude set; IllegalStateException when none is (hasLatitude). */
    public double getLatitude() {
        if (!mHasLatitude) throw new IllegalStateException();
        return mLatitude;
    }
    public void setLatitude(double latitude) { mLatitude = latitude; mHasLatitude = true; }
    public void clearLatitude() { mHasLatitude = false; }
    public boolean hasLongitude() { return mHasLongitude; }
    /** The longitude set; IllegalStateException when none is (hasLongitude). */
    public double getLongitude() {
        if (!mHasLongitude) throw new IllegalStateException();
        return mLongitude;
    }
    public void setLongitude(double longitude) { mLongitude = longitude; mHasLongitude = true; }
    public void clearLongitude() { mHasLongitude = false; }

    public Bundle getExtras() { return mExtras; }
    /** A copy of the extras is kept, not the Bundle given. */
    public void setExtras(Bundle extras) { mExtras = extras == null ? null : new Bundle(extras); }

    @Override public String toString() {
        StringBuilder b = new StringBuilder("Address[addressLines=[");
        for (int i = 0; i <= mMaxAddressLineIndex; i++) {
            if (i > 0) b.append(',');
            String line = mAddressLines.get(i);
            b.append(i).append(':').append(line == null ? "null" : '"' + line + '"');
        }
        return b.append("],feature=").append(mFeatureName).append(",admin=").append(mAdminArea).append(",sub-admin=").append(mSubAdminArea)
            .append(",locality=").append(mLocality).append(",thoroughfare=").append(mThoroughfare).append(",postalCode=").append(mPostalCode)
            .append(",countryCode=").append(mCountryCode).append(",countryName=").append(mCountryName).append(",hasLatitude=").append(mHasLatitude)
            .append(",latitude=").append(mLatitude).append(",hasLongitude=").append(mHasLongitude).append(",longitude=").append(mLongitude)
            .append(",phone=").append(mPhone).append(",url=").append(mUrl).append(",extras=").append(mExtras).append(']').toString();
    }

    public int describeContents() { return mExtras != null ? mExtras.describeContents() : 0; }

    public void writeToParcel(Parcel parcel, int flags) {
        parcel.writeString(mLocale == null ? "" : mLocale.getLanguage());
        parcel.writeString(mLocale == null ? "" : mLocale.getCountry());
        if (mAddressLines == null) {
            parcel.writeInt(0);
        } else {
            parcel.writeInt(mAddressLines.size());
            for (Map.Entry<Integer, String> e : mAddressLines.entrySet()) {
                parcel.writeInt(e.getKey());
                parcel.writeString(e.getValue());
            }
        }
        for (String s : new String[] { mFeatureName, mAdminArea, mSubAdminArea, mLocality, mSubLocality, mThoroughfare, mSubThoroughfare, mPremises,
                                       mPostalCode, mCountryCode, mCountryName })
            parcel.writeString(s);
        parcel.writeInt(mHasLatitude ? 1 : 0);
        if (mHasLatitude) parcel.writeDouble(mLatitude);
        parcel.writeInt(mHasLongitude ? 1 : 0);
        if (mHasLongitude) parcel.writeDouble(mLongitude);
        parcel.writeString(mPhone);
        parcel.writeString(mUrl);
        parcel.writeBundle(mExtras);
    }

    public static final Parcelable.Creator<Address> CREATOR = new Parcelable.Creator<Address>() {
        @Override public Address createFromParcel(Parcel in) {
            String language = in.readString(), country = in.readString();
            Address a = new Address(country != null && country.length() > 0 ? new Locale(language, country) : new Locale(language == null ? "" : language));
            int lines = in.readInt();
            if (lines > 0) {
                a.mAddressLines = new HashMap<>(lines);
                for (int i = 0; i < lines; i++) {
                    int index = in.readInt();
                    String line = in.readString();
                    a.mAddressLines.put(index, line);
                    a.mMaxAddressLineIndex = Math.max(a.mMaxAddressLineIndex, index);
                }
            }
            a.mFeatureName = in.readString();
            a.mAdminArea = in.readString();
            a.mSubAdminArea = in.readString();
            a.mLocality = in.readString();
            a.mSubLocality = in.readString();
            a.mThoroughfare = in.readString();
            a.mSubThoroughfare = in.readString();
            a.mPremises = in.readString();
            a.mPostalCode = in.readString();
            a.mCountryCode = in.readString();
            a.mCountryName = in.readString();
            a.mHasLatitude = in.readInt() != 0;
            if (a.mHasLatitude) a.mLatitude = in.readDouble();
            a.mHasLongitude = in.readInt() != 0;
            if (a.mHasLongitude) a.mLongitude = in.readDouble();
            a.mPhone = in.readString();
            a.mUrl = in.readString();
            a.mExtras = in.readBundle();
            return a;
        }
        @Override public Address[] newArray(int size) { return new Address[size]; }
    };
}
