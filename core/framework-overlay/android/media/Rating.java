// Overlay of core/framework/gen-src/android/media/Rating.java (tools/build-framework.sh).
// It differs in being a rating. The generated stub's five makers each gave an empty Rating -- style 0, unrated, no heart, no
// thumb, 0 stars -- and it went through a Parcel as nothing (CREATOR null); here it is Android's: a style and a value, the
// makers refusing what Android refuses (null for a star rating out of its style's range, a percentage outside 0..100, an
// unrated rating of no known style), the getters answering for their own style only (-1 for stars or a percentage asked
// of another style or of an unrated one), and its Parcel form.
//
// Why. A media session's metadata carries them (MediaMetadata.getRating, the user's heart on a track), a controller sets
// one (TransportControls.setRating, to the session's Callback.onSetRating), and androidx's RatingCompat is made from one
// by its style and value: every heart read as "no rating style". The members are the stub's.
package android.media;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class Rating implements android.os.Parcelable {
    public static final int RATING_3_STARS = 3;
    public static final int RATING_4_STARS = 4;
    public static final int RATING_5_STARS = 5;
    public static final int RATING_HEART = 1;
    public static final int RATING_NONE = 0;
    public static final int RATING_PERCENTAGE = 6;
    public static final int RATING_THUMB_UP_DOWN = 2;
    private static final float RATING_NOT_RATED = -1.0f;

    private final int mRatingStyle;
    private final float mRatingValue;

    private Rating(int ratingStyle, float rating) {
        mRatingStyle = ratingStyle;
        mRatingValue = rating;
    }

    public static android.media.Rating newHeartRating(boolean hasHeart) { return new Rating(RATING_HEART, hasHeart ? 1.0f : 0.0f); }
    /** Null for a percentage outside 0..100. */
    public static android.media.Rating newPercentageRating(float percent) {
        if (percent < 0.0f || percent > 100.0f) {
            android.util.Log.e("Rating", "Invalid percentage-based rating value");
            return null;
        }
        return new Rating(RATING_PERCENTAGE, percent);
    }
    /** Null for a style that is not one of the three star styles, or for more stars than the style has (or fewer than 0). */
    public static android.media.Rating newStarRating(int starRatingStyle, float starRating) {
        float maxRating;
        switch (starRatingStyle) {
            case RATING_3_STARS: maxRating = 3.0f; break;
            case RATING_4_STARS: maxRating = 4.0f; break;
            case RATING_5_STARS: maxRating = 5.0f; break;
            default:
                android.util.Log.e("Rating", "Invalid rating style (" + starRatingStyle + ") for a star rating");
                return null;
        }
        if (starRating < 0.0f || starRating > maxRating) {
            android.util.Log.e("Rating", "Trying to set out of range star-based rating");
            return null;
        }
        return new Rating(starRatingStyle, starRating);
    }
    public static android.media.Rating newThumbRating(boolean thumbIsUp) { return new Rating(RATING_THUMB_UP_DOWN, thumbIsUp ? 1.0f : 0.0f); }
    /** A rating of this style that says nothing yet; null for no known style. */
    public static android.media.Rating newUnratedRating(int ratingStyle) {
        switch (ratingStyle) {
            case RATING_HEART:
            case RATING_THUMB_UP_DOWN:
            case RATING_3_STARS:
            case RATING_4_STARS:
            case RATING_5_STARS:
            case RATING_PERCENTAGE:
                return new Rating(ratingStyle, RATING_NOT_RATED);
            default:
                return null;
        }
    }

    // (Android's own, odd as it is: the style.)
    public int describeContents() { return mRatingStyle; }
    /** The percentage; -1 for a rating of another style, or an unrated one. */
    public float getPercentRating() { return mRatingStyle != RATING_PERCENTAGE || !isRated() ? -1.0f : mRatingValue; }
    public int getRatingStyle() { return mRatingStyle; }
    /** The stars; -1 for a rating of another style, or an unrated one. */
    public float getStarRating() {
        switch (mRatingStyle) {
            case RATING_3_STARS:
            case RATING_4_STARS:
            case RATING_5_STARS:
                if (isRated()) return mRatingValue;
                return -1.0f;
            default:
                return -1.0f;
        }
    }
    public boolean hasHeart() { return mRatingStyle == RATING_HEART && mRatingValue == 1.0f; }
    public boolean isRated() { return mRatingValue >= 0.0f; }
    public boolean isThumbUp() { return mRatingStyle == RATING_THUMB_UP_DOWN && mRatingValue == 1.0f; }
    @Override public String toString() {
        return "Rating:style=" + mRatingStyle + " rating=" + (mRatingValue < 0.0f ? "unrated" : String.valueOf(mRatingValue));
    }
    public void writeToParcel(android.os.Parcel dest, int flags) {
        dest.writeInt(mRatingStyle);
        dest.writeFloat(mRatingValue);
    }
    public static final android.os.Parcelable.Creator<Rating> CREATOR = new android.os.Parcelable.Creator<Rating>() {
        public Rating createFromParcel(android.os.Parcel p) { return new Rating(p.readInt(), p.readFloat()); }
        public Rating[] newArray(int size) { return new Rating[size]; }
    };
}
