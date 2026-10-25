// Overlay of core/framework/src/android/database/AbstractCursor.java (tools/build-framework.sh).
// It differs in setNotificationUri, which registers an observer of the cursor's own with the resolver for that URI, as
// Android's does, so that a change the URI's provider tells (ContentResolver.notifyChange) reaches the cursor's content
// observers; close unregisters it. The original only kept the URI: a cursor never heard of a change -- Play Store's
// downloads' progress, a CursorLoader's data.
//
// Everything else is the original's, unchanged.
package android.database;

public abstract class AbstractCursor implements CrossProcessCursor {
    protected int mPos = -1;
    protected boolean mClosed;
    protected android.content.ContentResolver mContentResolver;
    private final DataSetObservable mDataSetObservable = new DataSetObservable();
    private final ContentObservable mContentObservable = new ContentObservable();
    private android.os.Bundle mExtras = android.os.Bundle.EMPTY;
    private android.net.Uri mNotifyUri;
    /* Registered with mContentResolver for mNotifyUri while the cursor is open: it tells the cursor's observers of a change. */
    private ContentObserver mSelfObserver;
    private static final class SelfObserver extends ContentObserver {
        private final java.lang.ref.WeakReference<AbstractCursor> cursor;
        SelfObserver(AbstractCursor c) { super(null); cursor = new java.lang.ref.WeakReference<>(c); }
        @Override public boolean deliverSelfNotifications() { return false; }
        @Override public void onChange(boolean self) { AbstractCursor c = cursor.get(); if (c != null) c.onChange(false); }
    }
    private void unregisterSelf() {
        ContentObserver o;
        synchronized (mContentObservable) { o = mSelfObserver; mSelfObserver = null; }
        if (o != null && mContentResolver != null) mContentResolver.unregisterContentObserver(o);
    }
    public abstract int getCount();
    public abstract String[] getColumnNames();
    public abstract String getString(int i);
    public abstract short getShort(int i);
    public abstract int getInt(int i);
    public abstract long getLong(int i);
    public abstract float getFloat(int i);
    public abstract double getDouble(int i);
    public abstract boolean isNull(int i);
    public int getType(int i) { return isNull(i) ? FIELD_TYPE_NULL : FIELD_TYPE_STRING; }
    public byte[] getBlob(int i) { throw new UnsupportedOperationException("getBlob is not supported"); }
    public CursorWindow getWindow() { return null; }
    public int getColumnCount() { return getColumnNames().length; }
    public void deactivate() { onDeactivateOrClose(); }
    protected void onDeactivateOrClose() { mDataSetObservable.notifyInvalidated(); }
    public boolean requery() { mDataSetObservable.notifyChanged(); return true; }
    public boolean isClosed() { return mClosed; }
    public void close() { mClosed = true; mContentObservable.unregisterAll(); unregisterSelf(); onDeactivateOrClose(); }
    public boolean onMove(int oldPos, int newPos) { return true; }
    public void copyStringToBuffer(int i, CharArrayBuffer b) { String s = getString(i); if (s != null) { char[] d = b.data; if (d == null || d.length < s.length()) b.data = s.toCharArray(); else s.getChars(0, s.length(), d, 0); b.sizeCopied = s.length(); } else b.sizeCopied = 0; }
    public final int getPosition() { return mPos; }
    public final boolean moveToPosition(int p) {
        int count = getCount();
        if (p >= count) { mPos = count; return false; }
        if (p < 0) { mPos = -1; return false; }
        if (p == mPos) return true;
        boolean r = onMove(mPos, p);
        mPos = r ? p : -1;
        return r;
    }
    public void fillWindow(int p, CursorWindow w) {}
    public final boolean move(int o) { return moveToPosition(mPos + o); }
    public final boolean moveToFirst() { return moveToPosition(0); }
    public final boolean moveToLast() { return moveToPosition(getCount() - 1); }
    public final boolean moveToNext() { return moveToPosition(mPos + 1); }
    public final boolean moveToPrevious() { return moveToPosition(mPos - 1); }
    public final boolean isFirst() { return mPos == 0 && getCount() != 0; }
    public final boolean isLast() { int c = getCount(); return mPos == c - 1 && c != 0; }
    public final boolean isBeforeFirst() { return getCount() == 0 || mPos == -1; }
    public final boolean isAfterLast() { return getCount() == 0 || mPos == getCount(); }
    public int getColumnIndex(String name) {
        int dot = name.lastIndexOf('.');
        if (dot != -1) name = name.substring(dot + 1);
        String[] cols = getColumnNames();
        for (int i = 0; i < cols.length; i++) if (cols[i].equalsIgnoreCase(name)) return i;
        return -1;
    }
    public int getColumnIndexOrThrow(String name) { int i = getColumnIndex(name); if (i < 0) throw new IllegalArgumentException("column '" + name + "' does not exist. Available columns: " + java.util.Arrays.toString(getColumnNames())); return i; }
    public String getColumnName(int i) { return getColumnNames()[i]; }
    public void registerContentObserver(ContentObserver o) { mContentObservable.registerObserver(o); }
    public void unregisterContentObserver(ContentObserver o) { if (!mClosed) mContentObservable.unregisterObserver(o); }
    public void registerDataSetObserver(DataSetObserver o) { mDataSetObservable.registerObserver(o); }
    public void unregisterDataSetObserver(DataSetObserver o) { mDataSetObservable.unregisterObserver(o); }
    protected void onChange(boolean self) { mContentObservable.dispatchChange(self, null); }
    public void setNotificationUri(android.content.ContentResolver r, android.net.Uri u) {
        unregisterSelf();
        mContentResolver = r;
        mNotifyUri = u;
        if (r == null || u == null || mClosed) return;
        ContentObserver o = new SelfObserver(this);
        synchronized (mContentObservable) { mSelfObserver = o; }
        r.registerContentObserver(u, true, o);
    }
    public android.net.Uri getNotificationUri() { return mNotifyUri; }
    public boolean getWantsAllOnMoveCalls() { return false; }
    public void setExtras(android.os.Bundle e) { mExtras = e == null ? android.os.Bundle.EMPTY : e; }
    public android.os.Bundle getExtras() { return mExtras; }
    public android.os.Bundle respond(android.os.Bundle e) { return android.os.Bundle.EMPTY; }
    protected void checkPosition() { if (-1 == mPos || getCount() == mPos) throw new CursorIndexOutOfBoundsException(mPos, getCount()); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    protected java.lang.Long mCurrentRowID;
    protected int mRowIdColumnIndex;
    protected java.util.HashMap mUpdatedRows;
    protected java.lang.Object getUpdatedField(int p0) { return null; }
    protected boolean isFieldUpdated(int p0) { return false; }
    public void setNotificationUris(android.content.ContentResolver p0, java.util.List p1, int p2, boolean p3) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static class SelfContentObserver extends android.database.ContentObserver {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public SelfContentObserver(android.database.AbstractCursor p0) { super((android.os.Handler) null); }
        public boolean deliverSelfNotifications() { return false; }
        public void onChange(boolean p0) {}
        SelfContentObserver() { this((android.database.AbstractCursor) null); }
    }
    // ---- end of generated nested classes
}
