// Overlay of core/framework/src/megler/GooglePackages.java (tools/build-framework.sh).
// It differs in one more package it answers for, the System WebView of the Android root, com.android.webview: installed,
// with the version and the files its APK has (android.webkit.WebView.getCurrentWebViewPackage, which reads them from the
// APK's manifest). And in Play Store's own meta-data, its derived APK id the newest there is (ownMetaData, which says why), and
// in two flags of Play Store's that test its own updates (ownVersionCode). The rest is the original's, the made-up
// com.google.android.webview included.
//
// Why. The package manager knows the app and, through this class, the packages it pretends are installed; every other name
// is NameNotFoundException. The WebView is a package of its own whose code runs in the app, and its code asks the package
// manager for itself: Chromium's crash handling lists the WebView's APK and library folders
// (org.chromium.components.crash.browser.PackagePaths, at the browser's start) from getPackageInfo of its own package name,
// and ended the WebView's start on the null it got for it.
package megler;

/** Google Play services and the Play Store, as an app sees them on a phone with Google: installed, enabled, recent, and signed with
 *  Google's release key (its public certificate, which every app signed with that key carries). Apps check this before using
 *  Google's libraries ("This app won't run without Google Play services"); the services themselves are what Megler answers in place. */
public final class GooglePackages {
    public static final String GMS = "com.google.android.gms", VENDING = "com.android.vending", GSF = "com.google.android.gsf",
        /* the system WebView, at the version WebView.getCurrentWebViewPackage and the user agent give (Notion checks it is recent) */
        WEBVIEW = "com.google.android.webview";
    /** Google's release certificate (SHA-1 38:91:8a:45:3d:07:19:93:54:f8:b1:9a:f0:5e:c6:56:2c:ed:57:88) */
    private static final String CERT = ""
        + "308204433082032ba003020102020900c2e08746644a308d300d06092a864886f70d01010405003074310b3009060355040613025553311330110603"
        + "550408130a43616c69666f726e6961311630140603550407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e63"
        + "2e3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964301e170d3038303832313233313333345a170d33363031"
        + "30373233313333345a3074310b3009060355040613025553311330110603550408130a43616c69666f726e6961311630140603550407130d4d6f756e"
        + "7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110300e060355040b1307416e64726f69643110300e06035504031307"
        + "416e64726f696430820120300d06092a864886f70d01010105000382010d00308201080282010100ab562e00d83ba208ae0a966f124e29da11f2ab56"
        + "d08f58e2cca91303e9b754d372f640a71b1dcb130967624e4656a7776a92193db2e5bfb724a91e77188b0e6a47a43b33d9609b77183145ccdf7b2e58"
        + "6674c9e1565b1f4c6a5955bff251a63dabf9c55c27222252e875e4f8154a645f897168c0b1bfc612eabf785769bb34aa7984dc7e2ea2764cae8307d8"
        + "c17154d7ee5f64a51a44a602c249054157dc02cd5f5c0e55fbef8519fbe327f0b1511692c5a06f19d18385f5c4dbc2d6b93f68cc2979c70e18ab9386"
        + "6b3bd5db8999552a0e3b4c99df58fb918bedc182ba35e003c1b4b10dd244a8ee24fffd333872ab5221985edab0fc0d0b145b6aa192858e79020103a3"
        + "81d93081d6301d0603551d0e04160414c77d8cc2211756259a7fd382df6be398e4d786a53081a60603551d2304819e30819b8014c77d8cc221175625"
        + "9a7fd382df6be398e4d786a5a178a4763074310b3009060355040613025553311330110603550408130a43616c69666f726e69613116301406035504"
        + "07130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110300e060355040b1307416e64726f69643110300e"
        + "06035504031307416e64726f6964820900c2e08746644a308d300c0603551d13040530030101ff300d06092a864886f70d010104050003820101006d"
        + "d252ceef85302c360aaace939bcff2cca904bb5d7a1661f8ae46b2994204d0ff4a68c7ed1a531ec4595a623ce60763b167297a7ae35712c407f208f0"
        + "cb109429124d7b106219c084ca3eb3f9ad5fb871ef92269a8be28bf16d44c8d9a08e6cb2f005bb3fe2cb96447e868e731076ad45b33f6009ea19c161"
        + "e62641aa99271dfd5228c5c587875ddb7f452758d661f6cc0cccb7352e424cc4365c523532f7325137593c4ae341f4db41edda0d0b1071a7c440f0fe"
        + "9ea01cb627ca674369d084bd2fd911ff06cdbf2cfa10dc0f893ae35762919048c7efc64c7144178342f70581c9de573af55b390dd7fdb9418631895d"
        + "5f759f30112687ff621410c069308a";
    public static boolean is(String pkg) { return GMS.equals(pkg) || VENDING.equals(pkg) || GSF.equals(pkg) || WEBVIEW.equals(pkg) || systemWebView(pkg) != null; }
    /** Megler: the Android root's WebView package when pkg names it, else null (see the top of the file). */
    private static android.content.pm.PackageInfo systemWebView(String pkg) {
        if (pkg == null || !pkg.startsWith("com.android.webview")) return null;
        android.content.pm.PackageInfo p = android.webkit.WebView.getCurrentWebViewPackage();
        return p != null && pkg.equals(p.packageName) ? p : null;
    }
    public static android.content.pm.Signature signature() { return new android.content.pm.Signature(CERT); }
    /*
     * Megler: the meta-data of the app's own package (ApplicationInfo.self): Play Store's with its derived APK id
     * (com.android.vending.derived.apk.id, the variant of a bundle Google's bundletool writes into each APK it makes) the newest
     * there is; any other package's, and the rest of Play Store's, as its manifest has them.
     *
     * Why. Play Store asks Google's server for its own binary and, at the same version, compares the variants. Megler brings the
     * standalone APK (derived id 1) and the server names the split APKs for this phone (derived id 5 on a Lumia 650), so Play
     * Store downloaded them, about 70 MB, to install itself again. It installs such an update only once none of its activities
     * has been resumed for ten seconds while its process runs ("waiting for market to close"); an app of the phone's own is
     * suspended within a second of being left and ends on Back. The update never went in, every start downloaded it again, and
     * the tries left 291 MB on a Lumia 650 (package 662). Told this, it skips the variant ("Local Derived Apk Id ... > Server
     * Derived Apk Id"). The server then offers no binary at all, a newer version neither (package 669, the flags below), so
     * Play Store gets newer only with Megler's package or an APK added to the library (docs/JAVA.md, "Installing, and the
     * phone's other apps").
     */
    public static android.os.Bundle ownMetaData(String pkg, android.os.Bundle meta) {
        final String derived = "com.android.vending.derived.apk.id";
        if (!VENDING.equals(pkg) || meta == null || !meta.containsKey(derived) || Native.flag("real-derived-id")) return meta;
        android.os.Bundle b = new android.os.Bundle(meta);
        b.putInt(derived, Integer.MAX_VALUE);
        return b;
    }
    /*
     * Megler: the version of the app's own package (PackageManager.getPackageInfo of itself): Play Store's that of an older
     * release, 52.9.21 (85292100), when its flag older-self is set; any other package's, and Play Store's without the flag, as
     * its manifest has it.
     *
     * Why. A test of Play Store's own updates. Play Store sends its version to Google's server in its user agent and compares
     * the server's offer with it, so a Play Store that says it is older stands for one the next release has overtaken. The flag
     * real-derived-id leaves its derived APK id as the APK has it, to compare. On a Lumia 650 (package 669) the server offered
     * such a Play Store nothing with the newest derived id (ownMetaData), and with the APK's own id 53.7.23, whole: no patch
     * from the 53.7.23 it has, so it had read the 52.9.21.
     */
    private static int sOlderSelf = -1;
    private static boolean olderSelf(String pkg) {
        if (!VENDING.equals(pkg)) return false;
        if (sOlderSelf < 0) sOlderSelf = Native.flag("older-self") ? 1 : 0;
        return sOlderSelf == 1;
    }
    public static int ownVersionCode(String pkg, int code) { return olderSelf(pkg) ? 85292100 : code; }
    public static String ownVersionName(String pkg, String name) { return olderSelf(pkg) ? "52.9.21-24 [0] [PR] 968642861" : name; }
    /*
     * The uid Google Play services and Google Services Framework share, as on Android (com.google.uid.shared): not the app's.
     * Google Play services finds the service a consent screen goes through (DefaultAuthDelegateService), takes the first
     * package of that service's uid and asks whether Google signed it; with the app's uid that package was the app, and it
     * refused Instagram its Google sign-in ("Cannot delegate to Service with different signature", package 566). The app's
     * uid still has their packages too (PackageManager.getPackagesForUid), for the calls Google Play services makes of
     * itself here, which come from the app's uid. In Google Play services' own package it is the app's uid.
     */
    public static final int SHARED_UID = 10014;
    public static int sharedUid() { return GMS.equals(Native.packageName()) ? android.os.Process.myUid() : SHARED_UID; }
    public static int versionCode(String pkg) { return VENDING.equals(pkg) ? 84651730 : WEBVIEW.equals(pkg) ? 733920700 : 270000000; }
    public static String versionName(String pkg) { return VENDING.equals(pkg) ? "46.5.17-31 [0] [PR] 750000000" : WEBVIEW.equals(pkg) ? "140.0.7339.207" : "27.00.00 (190400-750000000)"; }
    public static android.content.pm.ApplicationInfo applicationInfo(String pkg) {
        android.content.pm.PackageInfo microg = MicroG.packageInfoBasic(pkg);
        if (microg != null) return microg.applicationInfo;
        android.content.pm.PackageInfo gsf = Gsf.packageInfo(pkg);
        if (gsf != null) return gsf.applicationInfo;
        android.content.pm.PackageInfo webview = systemWebView(pkg);
        if (webview != null) return webview.applicationInfo;
        android.content.pm.ApplicationInfo a = new android.content.pm.ApplicationInfo();
        a.packageName = a.processName = pkg; a.enabled = true;
        a.flags = android.content.pm.ApplicationInfo.FLAG_SYSTEM | android.content.pm.ApplicationInfo.FLAG_INSTALLED | android.content.pm.ApplicationInfo.FLAG_HAS_CODE;
        a.targetSdkVersion = 35; a.minSdkVersion = 23; a.uid = GMS.equals(pkg) || GSF.equals(pkg) ? sharedUid() : 10101;
        a.sourceDir = a.publicSourceDir = "/product/priv-app/" + pkg + "/" + pkg + ".apk";
        a.dataDir = "/data/user/0/" + pkg;
        return a;
    }
    public static android.content.pm.PackageInfo packageInfo(String pkg) { return packageInfo(pkg, ~0); }
    /** As PackageManager.getPackageInfo(pkg, flags) gives it: Google Play services' read whole only where `flags` ask for its
        components or permissions (megler.MicroG.packageInfoBasic). */
    public static android.content.pm.PackageInfo packageInfo(String pkg, int flags) {
        final int fullManifest = android.content.pm.PackageManager.GET_ACTIVITIES | android.content.pm.PackageManager.GET_RECEIVERS
                             | android.content.pm.PackageManager.GET_SERVICES | android.content.pm.PackageManager.GET_PROVIDERS
                             | android.content.pm.PackageManager.GET_PERMISSIONS;
        android.content.pm.PackageInfo microg = (flags & fullManifest) != 0 ? MicroG.packageInfo(pkg) : MicroG.packageInfoBasic(pkg);
        if (microg != null) return microg;
        android.content.pm.PackageInfo gsf = Gsf.packageInfo(pkg);
        if (gsf != null) return gsf;
        android.content.pm.PackageInfo webview = systemWebView(pkg);
        if (webview != null) return webview;
        android.content.pm.PackageInfo p = new android.content.pm.PackageInfo();
        p.packageName = pkg; p.versionCode = versionCode(pkg); p.versionName = versionName(pkg);
        p.applicationInfo = applicationInfo(pkg);
        p.signatures = new android.content.pm.Signature[] { signature() };
        p.signingInfo = new android.content.pm.SigningInfo(p.signatures);
        p.firstInstallTime = p.lastUpdateTime = 1230768000000L;
        return p;
    }
}
