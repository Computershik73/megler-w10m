package megler;

import android.content.ContentProvider;
import android.os.Bundle;

/** Google Play services' content providers that apps call directly (Megler reports Play services as installed, see GooglePackages).
 *  The accounts provider answers like a phone with no Google account signed in: apps that cannot list accounts assume one exists
 *  (YouTube then runs its signed-in onboarding check, which fails and closes the app). */
public final class GmsProviders {
    private GmsProviders() {}
    public static ContentProvider forAuthority(String a) {
        // With microG, its own providers first: its GServices (com.google.android.gsf.gservices) holds the Android ID of its
        // check-in, which Play Store reads there. Google Services Framework's, mounted beside it for Google's own Play services,
        // never had one, and Play Store said "Unexpected android-id = 0" and "Server error" (package 570).
        if (MicroG.isMicroG()) {
            ContentProvider own = MicroG.provider(a);
            if (own != null) return own;
        }
        ContentProvider gsf = Gsf.provider(a);
        if (gsf != null) return gsf;
        ContentProvider microg = MicroG.provider(a);
        if (microg != null) return microg;
        if ("com.google.android.gms.auth.accounts".equals(a)) return Accounts.INSTANCE;
        return null;
    }
    static final class Accounts extends ContentProvider {
        static final Accounts INSTANCE = new Accounts();
        @Override public boolean onCreate() { return true; }
        @Override public Bundle call(String method, String arg, Bundle extras) {
            if ("get_accounts".equals(method)) { Bundle b = new Bundle(); b.putParcelableArray("accounts", new android.accounts.Account[0]); return b; }
            return null;
        }
        @Override public android.database.Cursor query(android.net.Uri u, String[] p, String s, String[] a, String o) { return null; }
        @Override public String getType(android.net.Uri u) { return null; }
        @Override public android.net.Uri insert(android.net.Uri u, android.content.ContentValues v) { return null; }
        @Override public int delete(android.net.Uri u, String s, String[] a) { return 0; }
        @Override public int update(android.net.Uri u, android.content.ContentValues v, String s, String[] a) { return 0; }
    }
}
