// Overlay of core/framework/src/android/content/IntentFilter.java (tools/build-framework.sh).
// It differs in keeping a filter's hosts and ports (AuthorityEntry, which matches a Uri's host and port as Android's does),
// its paths and its scheme-specific parts (android.os.PatternMatcher, overlaid), and telling of them -- countDataAuthorities,
// getDataAuthority, getHosts, getHostsList, countDataPaths, getDataPath, hasDataPath... -- where addDataAuthority,
// addDataPath and addDataSchemeSpecificPart dropped what they were given and those said nothing. Android's own Settings (AOSP
// 9, which Megler's package carries) lists the sites an app opens links to from its filters' hosts (Utils.getHandledDomains:
// getHostsList). How a filter matches an intent (match) is the original's, unchanged.
package android.content;

import java.util.ArrayList;
import java.util.Iterator;

public class IntentFilter implements android.os.Parcelable {
    public static final int SYSTEM_HIGH_PRIORITY = 1000, SYSTEM_LOW_PRIORITY = -1000, MATCH_CATEGORY_MASK = 0xfff0000, MATCH_ADJUSTMENT_MASK = 0xffff,
        MATCH_CATEGORY_EMPTY = 0x100000, MATCH_CATEGORY_SCHEME = 0x200000, MATCH_CATEGORY_HOST = 0x300000, MATCH_CATEGORY_PATH = 0x500000,
        MATCH_CATEGORY_TYPE = 0x600000, NO_MATCH_TYPE = -1, NO_MATCH_DATA = -2, NO_MATCH_ACTION = -3, NO_MATCH_CATEGORY = -4;
    public static class MalformedMimeTypeException extends Exception { public MalformedMimeTypeException() {} public MalformedMimeTypeException(String s) { super(s); } }
    private final ArrayList<String> mActions = new ArrayList<>(), mCategories = new ArrayList<>(), mSchemes = new ArrayList<>(), mTypes = new ArrayList<>();
    private final ArrayList<AuthorityEntry> mAuthorities = new ArrayList<>();
    private final ArrayList<android.os.PatternMatcher> mPaths = new ArrayList<>(), mSsps = new ArrayList<>();
    private int mPriority;
    public IntentFilter() {}
    public IntentFilter(String action) { addAction(action); }
    public IntentFilter(String action, String type) throws MalformedMimeTypeException { addAction(action); addDataType(type); }
    public IntentFilter(IntentFilter o) {
        mActions.addAll(o.mActions); mCategories.addAll(o.mCategories); mSchemes.addAll(o.mSchemes); mTypes.addAll(o.mTypes); mPriority = o.mPriority;
        mAuthorities.addAll(o.mAuthorities); mPaths.addAll(o.mPaths); mSsps.addAll(o.mSsps);
    }
    public static IntentFilter create(String action, String type) { try { return new IntentFilter(action, type); } catch (MalformedMimeTypeException e) { throw new RuntimeException(e); } }
    public final void setPriority(int p) { mPriority = p; }
    public final int getPriority() { return mPriority; }
    public final void addAction(String a) { if (a != null && !mActions.contains(a)) mActions.add(a); }
    public final int countActions() { return mActions.size(); }
    public final String getAction(int i) { return mActions.get(i); }
    public final boolean hasAction(String a) { return a != null && mActions.contains(a); }
    public final boolean matchAction(String a) { return hasAction(a); }
    public final Iterator<String> actionsIterator() { return mActions.iterator(); }
    public final void addCategory(String c) { if (!mCategories.contains(c)) mCategories.add(c); }
    public final int countCategories() { return mCategories.size(); }
    public final String getCategory(int i) { return mCategories.get(i); }
    public final boolean hasCategory(String c) { return mCategories.contains(c); }
    public final Iterator<String> categoriesIterator() { return mCategories.iterator(); }
    public final void addDataScheme(String s) { if (!mSchemes.contains(s)) mSchemes.add(s); }
    public final int countDataSchemes() { return mSchemes.size(); }
    public final String getDataScheme(int i) { return mSchemes.get(i); }
    public final boolean hasDataScheme(String s) { return mSchemes.contains(s); }
    public final void addDataType(String t) throws MalformedMimeTypeException { mTypes.add(t); }
    public final void addDataAuthority(String host, String port) {
        mAuthorities.add(new AuthorityEntry(host, port));
    }
    public final void addDataPath(String path, int type) {
        mPaths.add(new android.os.PatternMatcher(path, type));
    }
    public final void addDataSchemeSpecificPart(String ssp, int type) {
        mSsps.add(new android.os.PatternMatcher(ssp, type));
    }
    public final int countDataTypes() { return mTypes.size(); }
    public final boolean hasDataType(String t) { return mTypes.contains(t); }
    public final int match(String action, String type, String scheme, android.net.Uri data, java.util.Set<String> categories, String tag) { return hasAction(action) ? MATCH_CATEGORY_EMPTY : NO_MATCH_ACTION; }
    public final int match(ContentResolver r, Intent i, boolean resolve, String tag) { return match(i.getAction(), i.getType(), i.getScheme(), i.getData(), i.getCategories(), tag); }
    public int describeContents() { return 0; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    public static final long BLOCK_NULL_ACTION_INTENTS = 293560872L;
    public static android.os.Parcelable.Creator CREATOR;
    public static final int MATCH_ADJUSTMENT_NORMAL = 32768;
    public static final int MATCH_CATEGORY_PORT = 4194304;
    public static final int MATCH_CATEGORY_SCHEME_SPECIFIC_PART = 5767168;
    public static final int NO_MATCH_EXTRAS = -5;
    public static final java.lang.String SCHEME_HTTP = "http";
    public static final java.lang.String SCHEME_HTTPS = "https";
    public static final java.lang.String SCHEME_PACKAGE = "package";
    public static final int VISIBILITY_EXPLICIT = 1;
    public static final int VISIBILITY_IMPLICIT = 2;
    public static final int VISIBILITY_NONE = 0;
    public static final java.lang.String WILDCARD = "*";
    public static final java.lang.String WILDCARD_PATH = "/*";
    public IntentFilter(android.os.Parcel p0) { this(); }
    public static boolean filterEquals(android.content.IntentFilter p0, android.content.IntentFilter p1) { return false; }
    public void addDataAuthority(android.content.IntentFilter.AuthorityEntry p0) {
        mAuthorities.add(p0);
    }
    public void addDataPath(android.os.PatternMatcher p0) {
        mPaths.add(p0);
    }
    public void addDataSchemeSpecificPart(android.os.PatternMatcher p0) {
        mSsps.add(p0);
    }
    public void addDynamicDataType(java.lang.String p0) {}
    public void addExtra(java.lang.String p0, double p1) {}
    public void addExtra(java.lang.String p0, int p1) {}
    public void addExtra(java.lang.String p0, long p1) {}
    public void addExtra(java.lang.String p0, java.lang.String p1) {}
    public void addExtra(java.lang.String p0, boolean p1) {}
    public void addExtra(java.lang.String p0, double[] p1) {}
    public void addExtra(java.lang.String p0, int[] p1) {}
    public void addExtra(java.lang.String p0, long[] p1) {}
    public void addExtra(java.lang.String p0, java.lang.String[] p1) {}
    public void addExtra(java.lang.String p0, boolean[] p1) {}
    public void addMimeGroup(java.lang.String p0) {}
    public void addUriRelativeFilterGroup(android.content.UriRelativeFilterGroup p0) {}
    public java.util.function.Predicate asPredicate() { return null; }
    public java.util.function.Predicate asPredicateWithTypeResolution(android.content.ContentResolver p0) { return null; }
    public java.util.Iterator authoritiesIterator() { return null; }
    public boolean checkDataPathAndSchemeSpecificParts() { return false; }
    public void clearDynamicDataTypes() {}
    public void clearUriRelativeFilterGroups() {}
    public int countDataAuthorities() {
        return mAuthorities.size();
    }
    public int countDataPaths() {
        return mPaths.size();
    }
    public int countDataSchemeSpecificParts() {
        return mSsps.size();
    }
    public int countMimeGroups() { return 0; }
    public int countStaticDataTypes() { return 0; }
    public int countUriRelativeFilterGroups() { return 0; }
    public java.util.List dataTypes() { return new java.util.ArrayList(); }
    public boolean debugCheck() { return false; }
    public void dump(android.util.Printer p0, java.lang.String p1) {}
    public void dumpDebug(android.util.proto.ProtoOutputStream p0, long p1) {}
    public boolean getAutoVerify() { return (megler.Fill.get(meglerFill, "AutoVerify") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "AutoVerify") : false); }
    public boolean[] getBooleanArrayExtra(java.lang.String p0) { return null; }
    public boolean getBooleanExtra(java.lang.String p0) { return false; }
    public android.content.IntentFilter.AuthorityEntry getDataAuthority(int p0) {
        return mAuthorities.get(p0);
    }
    public android.os.PatternMatcher getDataPath(int p0) {
        return mPaths.get(p0);
    }
    public android.os.PatternMatcher getDataSchemeSpecificPart(int p0) {
        return mSsps.get(p0);
    }
    public java.lang.String getDataType(int p0) { return null; }
    public double[] getDoubleArrayExtra(java.lang.String p0) { return null; }
    public double getDoubleExtra(java.lang.String p0) { return 0d; }
    public android.os.PersistableBundle getExtras() { return (android.os.PersistableBundle) megler.Fill.get(meglerFill, "Extras"); }
    public java.lang.String[] getHosts() {
        java.util.ArrayList<String> l = getHostsList(); return l.isEmpty() ? null : l.toArray(new String[0]);
    }
    public java.util.ArrayList getHostsList() {
        java.util.ArrayList<String> l = new java.util.ArrayList<>(); for (AuthorityEntry a : mAuthorities) l.add(a.getHost()); return l;
    }
    public int[] getIntArrayExtra(java.lang.String p0) { return null; }
    public int getIntExtra(java.lang.String p0) { return 0; }
    public long[] getLongArrayExtra(java.lang.String p0) { return null; }
    public long getLongExtra(java.lang.String p0) { return 0L; }
    public java.lang.String getMimeGroup(int p0) { return null; }
    public int getOrder() { return (megler.Fill.get(meglerFill, "Order") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "Order") : 0); }
    public java.lang.String[] getStringArrayExtra(java.lang.String p0) { return null; }
    public java.lang.String getStringExtra(java.lang.String p0) { return null; }
    public android.content.UriRelativeFilterGroup getUriRelativeFilterGroup(int p0) { return null; }
    public int getVisibilityToInstantApp() { return (megler.Fill.get(meglerFill, "VisibilityToInstantApp") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "VisibilityToInstantApp") : 0); }
    public boolean handleAllWebDataURI() { return false; }
    public boolean handlesWebUris(boolean p0) { return false; }
    public boolean hasDataAuthority(android.content.IntentFilter.AuthorityEntry p0) {
        for (AuthorityEntry a : mAuthorities) if (a.match(p0)) return true; return false;
    }
    public boolean hasDataAuthority(android.net.Uri p0) { return false; }
    public boolean hasDataPath(android.os.PatternMatcher p0) {
        for (android.os.PatternMatcher m : mPaths) if (m.getType() == p0.getType() && m.getPath().equals(p0.getPath())) return true; return false;
    }
    public boolean hasDataPath(java.lang.String p0) {
        for (android.os.PatternMatcher m : mPaths) if (m.match(p0)) return true; return false;
    }
    public boolean hasDataSchemeSpecificPart(android.os.PatternMatcher p0) { return false; }
    public boolean hasDataSchemeSpecificPart(java.lang.String p0) {
        for (android.os.PatternMatcher m : mSsps) if (m.match(p0)) return true; return false;
    }
    public boolean hasExactDataType(java.lang.String p0) { return false; }
    public boolean hasExactDynamicDataType(java.lang.String p0) { return false; }
    public boolean hasExactStaticDataType(java.lang.String p0) { return false; }
    public boolean hasExtra(java.lang.String p0) { return false; }
    public boolean hasMimeGroup(java.lang.String p0) { return false; }
    public boolean isExplicitlyVisibleToInstantApp() { return false; }
    public boolean isImplicitlyVisibleToInstantApp() { return false; }
    public boolean isVerified() { return (megler.Fill.get(meglerFill, "Verified") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "Verified") : false); }
    public boolean isVisibleToInstantApp() { return false; }
    public int match(java.lang.String p0, java.lang.String p1, java.lang.String p2, android.net.Uri p3, java.util.Set p4, java.lang.String p5, boolean p6, java.util.Collection p7) { return 0; }
    public int match(java.lang.String p0, java.lang.String p1, java.lang.String p2, android.net.Uri p3, java.util.Set p4, java.lang.String p5, boolean p6, java.util.Collection p7, android.os.Bundle p8) { return 0; }
    // Megler: written as Android has it (it was a signature alone, which said every category matched): null when every
    // category of the intent's is the filter's, else the first one that is not. megler.MicroG asks it of Google Play
    // services' filters, whose categorised ones name the categories their callers give (DEFAULT, for the backup accounts'
    // and the reset protection's services).
    public java.lang.String matchCategories(java.util.Set<java.lang.String> categories) {
        if (categories == null) return null;
        for (java.lang.String c : categories) if (!mCategories.contains(c)) return c;
        return null;
    }
    public int matchData(java.lang.String p0, java.lang.String p1, android.net.Uri p2) { return 0; }
    public int matchDataAuthority(android.net.Uri p0) { return 0; }
    public int matchDataAuthority(android.net.Uri p0, boolean p1) { return 0; }
    public java.util.Iterator mimeGroupsIterator() { return null; }
    public boolean needsVerification() { return false; }
    public java.util.Iterator pathsIterator() { return null; }
    public void readFromXml(org.xmlpull.v1.XmlPullParser p0) {}
    public int safeCountActions() { return 0; }
    public java.util.Iterator schemeSpecificPartsIterator() { return null; }
    public java.util.Iterator schemesIterator() { return null; }
    public void setAutoVerify(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "AutoVerify", Boolean.valueOf(p0)); }
    public void setExtras(android.os.PersistableBundle p0) { meglerFill = megler.Fill.put(meglerFill, "Extras", p0); }
    public void setOrder(int p0) { meglerFill = megler.Fill.put(meglerFill, "Order", Integer.valueOf(p0)); }
    public void setVerified(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "Verified", Boolean.valueOf(p0)); }
    public void setVisibilityToInstantApp(int p0) { meglerFill = megler.Fill.put(meglerFill, "VisibilityToInstantApp", Integer.valueOf(p0)); }
    public java.lang.String toLongString() { return null; }
    public java.util.Iterator typesIterator() { return null; }
    public void writeToXml(org.xmlpull.v1.XmlSerializer p0) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    /** A host and its port, as a <data> names them: "*.example.com" any of its subdomains, port -1 any. */
    public static final class AuthorityEntry {
        private final String mHost, mOrigHost;
        private final boolean mWild;
        private final int mPort;
        public AuthorityEntry(java.lang.String host, java.lang.String port) {
            mOrigHost = host;
            mWild = host != null && host.length() > 0 && host.charAt(0) == '*';
            mHost = mWild ? host.substring(1) : host;
            mPort = port != null ? Integer.parseInt(port) : -1;
        }
        public java.lang.String getHost() { return mOrigHost; }
        public int getPort() { return mPort; }
        public int match(android.net.Uri data) { return match(data, false); }
        public int match(android.net.Uri data, boolean wildcardSupported) {
            String host = data.getHost();
            if (host == null || mHost == null) return NO_MATCH_DATA;
            if (!(wildcardSupported && "*".equals(mOrigHost))) {
                if (mWild) {
                    if (host.length() < mHost.length()) return NO_MATCH_DATA;
                    host = host.substring(host.length() - mHost.length());
                }
                if (host.compareToIgnoreCase(mHost) != 0) return NO_MATCH_DATA;
            }
            if (mPort >= 0) return mPort == data.getPort() ? MATCH_CATEGORY_PORT : NO_MATCH_DATA;
            return MATCH_CATEGORY_HOST;
        }
        public boolean match(android.content.IntentFilter.AuthorityEntry other) {
            if (other == null || mWild != other.mWild || mPort != other.mPort) return false;
            return mHost == null ? other.mHost == null : mHost.equals(other.mHost);
        }
        AuthorityEntry() { this((java.lang.String) null, (java.lang.String) null); }
    }
    // ---- end of generated nested classes
}
