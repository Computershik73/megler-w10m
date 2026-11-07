package android.content;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;
import java.util.ArrayList;
import java.util.Map;
import java.util.TreeMap;

/**
 * One operation of a batch (ContentResolver.applyBatch), as Android's: an insert, an update, a delete, an assertion or a
 * call, built by its Builder, with values, selection arguments and extras that may refer back to the results of the
 * operations before it. It was signatures alone, every Builder null: Viber (packages 575 to 645) ended when the user
 * tapped the button of its welcome screen, building ContentProviderOperation.newDelete(...).build() as it reset its data.
 */
public class ContentProviderOperation implements Parcelable {
    public static final int TYPE_INSERT = 1;
    public static final int TYPE_UPDATE = 2;
    public static final int TYPE_DELETE = 3;
    public static final int TYPE_ASSERT = 4;
    public static final int TYPE_CALL = 5;

    private final int mType;
    private final Uri mUri;
    private final String mMethod, mArg;
    private final Map<String, Object> mValues, mExtras;   /* a value may be a BackReference */
    private final String mSelection;
    private final TreeMap<Integer, Object> mSelectionArgs;
    private final Integer mExpectedCount;
    private final boolean mYieldAllowed, mExceptionAllowed;

    private ContentProviderOperation(Builder b) {
        mType = b.mType;
        mUri = b.mUri;
        mMethod = b.mMethod;
        mArg = b.mArg;
        mValues = b.mValues;
        mExtras = b.mExtras;
        mSelection = b.mSelection;
        mSelectionArgs = b.mSelectionArgs;
        mExpectedCount = b.mExpectedCount;
        mYieldAllowed = b.mYieldAllowed;
        mExceptionAllowed = b.mExceptionAllowed;
    }
    /** The same operation on another URI (Android's, for a provider that passes a batch on). */
    public ContentProviderOperation(ContentProviderOperation o, Uri u) {
        mType = o.mType;
        mUri = u;
        mMethod = o.mMethod;
        mArg = o.mArg;
        mValues = o.mValues;
        mExtras = o.mExtras;
        mSelection = o.mSelection;
        mSelectionArgs = o.mSelectionArgs;
        mExpectedCount = o.mExpectedCount;
        mYieldAllowed = o.mYieldAllowed;
        mExceptionAllowed = o.mExceptionAllowed;
    }
    /** Made for Parcel's sake alone; every field empty. */
    public ContentProviderOperation() { this(new Builder(TYPE_DELETE, null)); }

    public static Builder newInsert(Uri u) { return new Builder(TYPE_INSERT, u); }
    public static Builder newUpdate(Uri u) { return new Builder(TYPE_UPDATE, u); }
    public static Builder newDelete(Uri u) { return new Builder(TYPE_DELETE, u); }
    public static Builder newAssertQuery(Uri u) { return new Builder(TYPE_ASSERT, u); }
    public static Builder newCall(Uri u, String method, String arg) { Builder b = new Builder(TYPE_CALL, u); b.mMethod = method; b.mArg = arg; return b; }

    public Uri getUri() { return mUri; }
    public int getType() { return mType; }
    public boolean isYieldAllowed() { return mYieldAllowed; }
    public boolean isExceptionAllowed() { return mExceptionAllowed; }
    public boolean isInsert() { return mType == TYPE_INSERT; }
    public boolean isDelete() { return mType == TYPE_DELETE; }
    public boolean isUpdate() { return mType == TYPE_UPDATE; }
    public boolean isAssertQuery() { return mType == TYPE_ASSERT; }
    public boolean isCall() { return mType == TYPE_CALL; }
    public boolean isWriteOperation() { return mType == TYPE_DELETE || mType == TYPE_INSERT || mType == TYPE_UPDATE; }
    public boolean isReadOperation() { return mType == TYPE_ASSERT; }

    /** Applies it to the provider, as Android's: an exception becomes the result where the builder allowed it. */
    public ContentProviderResult apply(ContentProvider provider, ContentProviderResult[] backRefs, int numBackRefs) throws OperationApplicationException {
        if (!mExceptionAllowed) return applyInternal(provider, backRefs, numBackRefs);
        try {
            return applyInternal(provider, backRefs, numBackRefs);
        } catch (Exception e) {
            return new ContentProviderResult(e);
        }
    }
    private ContentProviderResult applyInternal(ContentProvider provider, ContentProviderResult[] backRefs, int numBackRefs) throws OperationApplicationException {
        ContentValues values = resolveValueBackReferences(backRefs, numBackRefs);
        String[] args = resolveSelectionArgsBackReferences(backRefs, numBackRefs);
        if (mType == TYPE_INSERT) {
            Uri made = provider.insert(mUri, values);
            if (made == null) throw new OperationApplicationException("Insert into " + mUri + " returned no result");
            return new ContentProviderResult(made);
        }
        if (mType == TYPE_CALL) {
            Bundle r = provider.call(mUri.getAuthority(), mMethod, mArg, resolveExtrasBackReferences(backRefs, numBackRefs));
            return new ContentProviderResult(null, null, r, null);
        }
        int rows;
        if (mType == TYPE_DELETE) rows = provider.delete(mUri, mSelection, args);
        else if (mType == TYPE_UPDATE) rows = provider.update(mUri, values, mSelection, args);
        else if (mType == TYPE_ASSERT) {
            // The rows that match must hold the values expected, each read as a string.
            String[] projection = null;
            if (values != null) projection = values.keySet().toArray(new String[0]);
            Cursor c = provider.query(mUri, projection, mSelection, args, null);
            if (c == null) rows = 0;
            else {
                try {
                    rows = c.getCount();
                    if (projection != null) {
                        while (c.moveToNext()) {
                            for (int i = 0; i < projection.length; i++) {
                                String have = c.getString(i), want = values.getAsString(projection[i]);
                                if (!TextUtils.equals(have, want))
                                    throw new OperationApplicationException("Found value " + have + " when expected " + want + " for column " + projection[i]);
                            }
                        }
                    }
                } finally {
                    c.close();
                }
            }
        } else {
            throw new IllegalStateException("bad type, " + mType);
        }
        if (mExpectedCount != null && mExpectedCount != rows) throw new OperationApplicationException("Expected " + mExpectedCount + " rows but actual " + rows);
        return new ContentProviderResult(rows);
    }

    public ContentValues resolveValueBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mValues == null) return null;
        ContentValues v = new ContentValues();
        for (Map.Entry<String, Object> e : mValues.entrySet()) {
            Object o = e.getValue();
            v.putObject(e.getKey(), o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return v;
    }
    public Bundle resolveExtrasBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mExtras == null) return null;
        Bundle b = new Bundle();
        for (Map.Entry<String, Object> e : mExtras.entrySet()) {
            Object o = e.getValue();
            b.putObject(e.getKey(), o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return b;
    }
    public String[] resolveSelectionArgsBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mSelectionArgs == null) return null;
        String[] args = new String[mSelectionArgs.isEmpty() ? 0 : mSelectionArgs.lastKey() + 1];
        for (Map.Entry<Integer, Object> e : mSelectionArgs.entrySet()) {
            Object o = e.getValue();
            args[e.getKey()] = String.valueOf(o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return args;
    }

    public static String typeToString(int type) {
        switch (type) {
        case TYPE_INSERT: return "insert";
        case TYPE_UPDATE: return "update";
        case TYPE_DELETE: return "delete";
        case TYPE_ASSERT: return "assert";
        case TYPE_CALL: return "call";
        default: return Integer.toString(type);
        }
    }
    @Override public String toString() {
        StringBuilder s = new StringBuilder("ContentProviderOperation(");
        s.append("type=").append(typeToString(mType)).append(' ');
        if (mUri != null) s.append("uri=").append(mUri).append(' ');
        if (mValues != null) s.append("values=").append(mValues).append(' ');
        if (mSelection != null) s.append("selection=").append(mSelection).append(' ');
        if (mSelectionArgs != null) s.append("selectionArgs=").append(mSelectionArgs).append(' ');
        if (mExpectedCount != null) s.append("expectedCount=").append(mExpectedCount).append(' ');
        if (mYieldAllowed) s.append("yieldAllowed ");
        if (mExceptionAllowed) s.append("exceptionAllowed ");
        s.setLength(s.length() - 1);
        return s.append(')').toString();
    }

    // In this one process a Parcel carries objects as they are: the operation itself goes through it.
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel p, int flags) { p.writeValue(this); }
    public static final Parcelable.Creator<ContentProviderOperation> CREATOR = new Parcelable.Creator<ContentProviderOperation>() {
        public ContentProviderOperation createFromParcel(Parcel p) { return (ContentProviderOperation) p.readValue(null); }
        public ContentProviderOperation[] newArray(int n) { return new ContentProviderOperation[n]; }
    };

    /** A value taken from an earlier operation's result: its extras' key, else its URI's id, else its count. */
    public static class BackReference implements Parcelable {
        private final int mFromIndex;
        private final String mFromKey;
        private BackReference(int fromIndex, String fromKey) { mFromIndex = fromIndex; mFromKey = fromKey; }
        public BackReference(Parcel p) { BackReference b = (BackReference) p.readValue(null); mFromIndex = b == null ? 0 : b.mFromIndex; mFromKey = b == null ? null : b.mFromKey; }
        BackReference() { this(0, null); }
        public Object resolve(ContentProviderResult[] backRefs, int numBackRefs) {
            if (mFromIndex >= numBackRefs) throw new ArrayIndexOutOfBoundsException("asked for back ref " + mFromIndex + " but there are only " + numBackRefs + " back refs");
            ContentProviderResult r = backRefs[mFromIndex];
            if (r.extras != null) return r.extras.get(mFromKey);
            if (r.uri != null) return ContentUris.parseId(r.uri);
            return r.count == null ? null : (Object) Long.valueOf(r.count);
        }
        @Override public String toString() { return "{fromIndex=" + mFromIndex + ", fromKey=" + mFromKey + "}"; }
        public int describeContents() { return 0; }
        public void writeToParcel(Parcel p, int flags) { p.writeValue(this); }
        public static final Parcelable.Creator<BackReference> CREATOR = new Parcelable.Creator<BackReference>() {
            public BackReference createFromParcel(Parcel p) { return (BackReference) p.readValue(null); }
            public BackReference[] newArray(int n) { return new BackReference[n]; }
        };
    }

    /** Android's builder, with its checks of what each kind of operation may have. */
    public static class Builder {
        private final int mType;
        private final Uri mUri;
        private String mMethod, mArg;
        private Map<String, Object> mValues, mExtras;
        private String mSelection;
        private TreeMap<Integer, Object> mSelectionArgs;
        private Integer mExpectedCount;
        private boolean mYieldAllowed, mExceptionAllowed;

        private Builder(int type, Uri uri) { mType = type; mUri = uri; }
        protected Builder() { this(TYPE_DELETE, null); }

        public ContentProviderOperation build() {
            if (mType == TYPE_UPDATE && (mValues == null || mValues.isEmpty())) throw new IllegalArgumentException("Empty values");
            if (mType == TYPE_ASSERT && (mValues == null || mValues.isEmpty()) && mExpectedCount == null) throw new IllegalArgumentException("Empty values");
            return new ContentProviderOperation(this);
        }
        private void valuesAllowed() {
            if (mType != TYPE_INSERT && mType != TYPE_UPDATE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Values not supported for " + typeToString(mType));
            if (mValues == null) mValues = new java.util.LinkedHashMap<>();
        }
        private void selectionAllowed() {
            if (mType != TYPE_UPDATE && mType != TYPE_DELETE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Selection not supported for " + typeToString(mType));
            if (mSelectionArgs == null) mSelectionArgs = new TreeMap<>();
        }
        private void extrasAllowed() {
            if (mType == TYPE_ASSERT) throw new IllegalArgumentException("Extras not supported for " + typeToString(mType));
            if (mExtras == null) mExtras = new java.util.LinkedHashMap<>();
        }
        public Builder withValues(ContentValues values) {
            valuesAllowed();
            for (String k : values.keySet()) mValues.put(k, values.get(k));
            return this;
        }
        public Builder withValue(String key, Object value) {
            valuesAllowed();
            if (!ContentValues.isSupportedValue(value)) throw new IllegalArgumentException("bad value type: " + value.getClass().getName());
            mValues.put(key, value);
            return this;
        }
        public Builder withValueBackReferences(ContentValues backReferences) {
            valuesAllowed();
            for (String k : backReferences.keySet()) mValues.put(k, new BackReference(backReferences.getAsInteger(k), null));
            return this;
        }
        public Builder withValueBackReference(String key, int fromIndex) { valuesAllowed(); mValues.put(key, new BackReference(fromIndex, null)); return this; }
        public Builder withValueBackReference(String key, int fromIndex, String fromKey) { valuesAllowed(); mValues.put(key, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withExtras(Bundle extras) {
            extrasAllowed();
            for (String k : extras.keySet()) mExtras.put(k, extras.get(k));
            return this;
        }
        public Builder withExtra(String key, Object value) { extrasAllowed(); mExtras.put(key, value); return this; }
        public Builder withExtraBackReference(String key, int fromIndex) { extrasAllowed(); mExtras.put(key, new BackReference(fromIndex, null)); return this; }
        public Builder withExtraBackReference(String key, int fromIndex, String fromKey) { extrasAllowed(); mExtras.put(key, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withSelection(String selection, String[] selectionArgs) {
            selectionAllowed();
            mSelection = selection;
            if (selectionArgs == null) mSelectionArgs = null;
            else for (int i = 0; i < selectionArgs.length; i++) mSelectionArgs.put(i, selectionArgs[i]);
            return this;
        }
        public Builder withSelectionBackReference(int index, int fromIndex) { selectionAllowed(); mSelectionArgs.put(index, new BackReference(fromIndex, null)); return this; }
        public Builder withSelectionBackReference(int index, int fromIndex, String fromKey) { selectionAllowed(); mSelectionArgs.put(index, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withExpectedCount(int count) {
            if (mType != TYPE_UPDATE && mType != TYPE_DELETE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Expected count not supported for " + typeToString(mType));
            mExpectedCount = count;
            return this;
        }
        public Builder withYieldAllowed(boolean yieldAllowed) { mYieldAllowed = yieldAllowed; return this; }
        public Builder withExceptionAllowed(boolean exceptionAllowed) { mExceptionAllowed = exceptionAllowed; return this; }
        public Builder withFailureAllowed(boolean failureAllowed) { return withExceptionAllowed(failureAllowed); }
    }
}
