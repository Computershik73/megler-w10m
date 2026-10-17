// megler-w10m overlay: android.media.MediaMetadata, in place of the generated stub (Megler/src/java-framework/gen-src).
//
// Why. The stub held nothing: its getters answered null and 0, writeToParcel wrote nothing, and CREATOR was null.
// AndroidX's (and the support library's) MediaMetadataCompat makes the platform's object by writing itself into a
// Parcel and reading that back with MediaMetadata.CREATOR, in getMediaMetadata(), which MediaSessionCompat calls for
// every setMetadata. Spotify's "MediaSession worker" thread threw a NullPointerException on CREATOR.createFromParcel
// for each track's metadata, dozens of times a minute, and the session never had any.
//
// This is Android's class, as AOSP has it: the metadata is a Bundle of the keys; the Builder puts strings, texts,
// longs, bitmaps and ratings into it; the Parcel form is the Bundle and then the bitmap size limit, which a parcel
// written by MediaMetadataCompat (the Bundle alone) leaves at its default; getDescription makes a MediaDescription
// of the display keys, falling back on the title, artist and album as Android does.
package android.media;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Set;

public final class MediaMetadata implements Parcelable {
    public static final String METADATA_KEY_ALBUM = "android.media.metadata.ALBUM";
    public static final String METADATA_KEY_ALBUM_ART = "android.media.metadata.ALBUM_ART";
    public static final String METADATA_KEY_ALBUM_ARTIST = "android.media.metadata.ALBUM_ARTIST";
    public static final String METADATA_KEY_ALBUM_ART_URI = "android.media.metadata.ALBUM_ART_URI";
    public static final String METADATA_KEY_ART = "android.media.metadata.ART";
    public static final String METADATA_KEY_ARTIST = "android.media.metadata.ARTIST";
    public static final String METADATA_KEY_ART_URI = "android.media.metadata.ART_URI";
    public static final String METADATA_KEY_AUTHOR = "android.media.metadata.AUTHOR";
    public static final String METADATA_KEY_BT_FOLDER_TYPE = "android.media.metadata.BT_FOLDER_TYPE";
    public static final String METADATA_KEY_COMPILATION = "android.media.metadata.COMPILATION";
    public static final String METADATA_KEY_COMPOSER = "android.media.metadata.COMPOSER";
    public static final String METADATA_KEY_DATE = "android.media.metadata.DATE";
    public static final String METADATA_KEY_DISC_NUMBER = "android.media.metadata.DISC_NUMBER";
    public static final String METADATA_KEY_DISPLAY_DESCRIPTION = "android.media.metadata.DISPLAY_DESCRIPTION";
    public static final String METADATA_KEY_DISPLAY_ICON = "android.media.metadata.DISPLAY_ICON";
    public static final String METADATA_KEY_DISPLAY_ICON_URI = "android.media.metadata.DISPLAY_ICON_URI";
    public static final String METADATA_KEY_DISPLAY_SUBTITLE = "android.media.metadata.DISPLAY_SUBTITLE";
    public static final String METADATA_KEY_DISPLAY_TITLE = "android.media.metadata.DISPLAY_TITLE";
    public static final String METADATA_KEY_DURATION = "android.media.metadata.DURATION";
    public static final String METADATA_KEY_GENRE = "android.media.metadata.GENRE";
    public static final String METADATA_KEY_MEDIA_ID = "android.media.metadata.MEDIA_ID";
    public static final String METADATA_KEY_MEDIA_URI = "android.media.metadata.MEDIA_URI";
    public static final String METADATA_KEY_NUM_TRACKS = "android.media.metadata.NUM_TRACKS";
    public static final String METADATA_KEY_RATING = "android.media.metadata.RATING";
    public static final String METADATA_KEY_TITLE = "android.media.metadata.TITLE";
    public static final String METADATA_KEY_TRACK_NUMBER = "android.media.metadata.TRACK_NUMBER";
    public static final String METADATA_KEY_USER_RATING = "android.media.metadata.USER_RATING";
    public static final String METADATA_KEY_WRITER = "android.media.metadata.WRITER";
    public static final String METADATA_KEY_YEAR = "android.media.metadata.YEAR";

    /* The editor's numeric keys (MediaMetadataEditor, MediaMetadataRetriever's METADATA_KEY_*) and the names they stand for. */
    private static final int[] EDITOR_KEYS = { 1, 13, 2, 3, 7, 4, 5, 6, 8, 9, 11, 14, 0, 10, 15, 268435457, 101 };
    private static final String[] EDITOR_NAMES = { METADATA_KEY_ALBUM, METADATA_KEY_ALBUM_ARTIST, METADATA_KEY_ARTIST, METADATA_KEY_AUTHOR,
            METADATA_KEY_TITLE, METADATA_KEY_COMPOSER, METADATA_KEY_DATE, METADATA_KEY_GENRE, METADATA_KEY_YEAR, METADATA_KEY_DURATION,
            METADATA_KEY_WRITER, METADATA_KEY_DISC_NUMBER, METADATA_KEY_TRACK_NUMBER, METADATA_KEY_NUM_TRACKS, METADATA_KEY_COMPILATION,
            METADATA_KEY_USER_RATING, METADATA_KEY_ALBUM_ART };

    private final Bundle mBundle;
    private final int mBitmapDimensionLimit;
    private MediaDescription mDescription;

    private MediaMetadata(Bundle bundle, int bitmapDimensionLimit) {
        mBundle = new Bundle(bundle);
        mBitmapDimensionLimit = bitmapDimensionLimit;
    }

    private MediaMetadata(Parcel in) {
        Bundle b = in.readBundle();
        mBundle = b == null ? new Bundle() : b;
        mBitmapDimensionLimit = Math.max(in.readInt(), 1);
    }

    public static String getKeyFromMetadataEditorKey(int editorKey) {
        for (int i = 0; i < EDITOR_KEYS.length; i++) if (EDITOR_KEYS[i] == editorKey) return EDITOR_NAMES[i];
        return null;
    }

    public boolean containsKey(String key) { return mBundle.containsKey(key); }

    public CharSequence getText(String key) { return mBundle.getCharSequence(key); }

    public String getString(String key) {
        CharSequence text = getText(key);
        return text == null ? null : text.toString();
    }

    public long getLong(String key) { return mBundle.getLong(key, 0); }

    public Rating getRating(String key) {
        try {
            return (Rating) mBundle.getParcelable(key);
        } catch (RuntimeException e) {
            return null;
        }
    }

    public Bitmap getBitmap(String key) {
        try {
            return (Bitmap) mBundle.getParcelable(key);
        } catch (RuntimeException e) {
            return null;
        }
    }

    public int getBitmapDimensionLimit() { return mBitmapDimensionLimit; }

    public int size() { return mBundle.size(); }

    public Set<String> keySet() { return mBundle.keySet(); }

    /** The display keys, or failing them the first of the title, artist, album, album artist, writer, author and composer that are there. */
    public MediaDescription getDescription() {
        if (mDescription != null) return mDescription;
        String mediaId = getString(METADATA_KEY_MEDIA_ID);
        CharSequence[] text = new CharSequence[3];
        CharSequence displayText = getText(METADATA_KEY_DISPLAY_TITLE);
        if (displayText != null && displayText.length() > 0) {
            text[0] = displayText;
            text[1] = getText(METADATA_KEY_DISPLAY_SUBTITLE);
            text[2] = getText(METADATA_KEY_DISPLAY_DESCRIPTION);
        } else {
            String[] preferred = { METADATA_KEY_TITLE, METADATA_KEY_ARTIST, METADATA_KEY_ALBUM, METADATA_KEY_ALBUM_ARTIST, METADATA_KEY_WRITER,
                    METADATA_KEY_AUTHOR, METADATA_KEY_COMPOSER };
            int t = 0;
            for (int k = 0; t < text.length && k < preferred.length; k++) {
                CharSequence next = getText(preferred[k]);
                if (next != null && next.length() > 0) text[t++] = next;
            }
        }
        Bitmap icon = null;
        for (String key : new String[] { METADATA_KEY_DISPLAY_ICON, METADATA_KEY_ART, METADATA_KEY_ALBUM_ART }) {
            icon = getBitmap(key);
            if (icon != null) break;
        }
        Uri iconUri = null;
        for (String key : new String[] { METADATA_KEY_DISPLAY_ICON_URI, METADATA_KEY_ART_URI, METADATA_KEY_ALBUM_ART_URI }) {
            String s = getString(key);
            if (s != null && !s.isEmpty()) {
                iconUri = Uri.parse(s);
                break;
            }
        }
        String mediaUri = getString(METADATA_KEY_MEDIA_URI);
        mDescription = new MediaDescription.Builder().setMediaId(mediaId).setTitle(text[0]).setSubtitle(text[1]).setDescription(text[2])
                .setIconBitmap(icon).setIconUri(iconUri).setMediaUri(mediaUri == null || mediaUri.isEmpty() ? null : Uri.parse(mediaUri)).build();
        return mDescription;
    }

    @Override
    public int describeContents() { return 0; }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeBundle(mBundle);
        dest.writeInt(mBitmapDimensionLimit);
    }

    @Override
    public boolean equals(Object o) {
        if (o == this) return true;
        if (!(o instanceof MediaMetadata)) return false;
        MediaMetadata m = (MediaMetadata) o;
        if (m.size() != size()) return false;
        for (String key : keySet()) {
            Object a = mBundle.get(key), b = m.mBundle.get(key);
            if (a == null ? b != null : !a.equals(b)) return false;
        }
        return true;
    }

    @Override
    public int hashCode() {
        int h = 17;
        for (String key : keySet()) h = 31 * h + key.hashCode();
        return h;
    }

    @Override
    public String toString() { return "MediaMetadata{" + getDescription() + "}"; }

    public static final Parcelable.Creator<MediaMetadata> CREATOR = new Parcelable.Creator<MediaMetadata>() {
        @Override
        public MediaMetadata createFromParcel(Parcel in) { return new MediaMetadata(in); }

        @Override
        public MediaMetadata[] newArray(int size) { return new MediaMetadata[size]; }
    };

    public static final class Builder {
        private final Bundle mBundle;
        private int mBitmapDimensionLimit = Integer.MAX_VALUE;

        public Builder() { mBundle = new Bundle(); }

        public Builder(MediaMetadata source) {
            mBundle = new Bundle(source.mBundle);
            mBitmapDimensionLimit = source.mBitmapDimensionLimit;
        }

        public Builder putText(String key, CharSequence value) {
            mBundle.putCharSequence(key, value);
            return this;
        }

        public Builder putString(String key, String value) {
            mBundle.putCharSequence(key, value);
            return this;
        }

        public Builder putLong(String key, long value) {
            mBundle.putLong(key, value);
            return this;
        }

        public Builder putRating(String key, Rating value) {
            mBundle.putParcelable(key, value);
            return this;
        }

        public Builder putBitmap(String key, Bitmap value) {
            mBundle.putParcelable(key, value);
            return this;
        }

        public Builder setBitmapDimensionLimit(int limit) {
            mBitmapDimensionLimit = limit > 0 ? limit : Integer.MAX_VALUE;
            return this;
        }

        public MediaMetadata build() { return new MediaMetadata(mBundle, mBitmapDimensionLimit); }
    }
}
