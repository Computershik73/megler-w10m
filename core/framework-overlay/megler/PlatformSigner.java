// Megler: new in the overlay. The certificate the platform is signed with, which PackageManager gives as the package
// "android"'s signatures (getPackageInfo("android", GET_SIGNATURES or GET_SIGNING_CERTIFICATES)).
//
// What it is. The certificate of the platform's resources APK (framework-res.apk, the APK that is the package "android" on
// a phone; number 1 among the APKs the core reads): the X.509 certificates of its META-INF signature file (CERT.RSA; the
// APK is signed the old way, with no signing block), read once. An app signed with it would be a platform app, and none is.
//
// Why. The package "android" had one signature of no bytes. Code that asks for the platform's certificate to tell a
// platform app from another reads the bytes as a certificate: microG's location service (inside Google Play services) asks
// at every location it has for an app whether the app is Google's or the platform's (ExtendedPackageInfo.isPlatformPackage:
// getPackageInfo("android", GET_SIGNATURES), each signature wrapped in a CertData, which refuses fewer than 25 bytes with
// "CertData to small") -- the exception ended the app's request for locations ("W/LocationManager: Exception while
// processing for WorkSource{}: CertData to small", then the request cancelled and removed), and an app that asked Google
// Play services for locations (FusedLocationProviderClient) under microG never had one: Citymapper (0.1.0.690) showed no
// position and gave no routes from where the user is.
package megler;

public final class PlatformSigner {
    private static android.content.pm.Signature[] sSignatures;

    private PlatformSigner() {}

    /**
     * The platform's signatures, a new array each time; one of no bytes where the platform's APK has no certificate to read
     * (what the package "android" always had).
     */
    public static synchronized android.content.pm.Signature[] signatures() {
        if (sSignatures == null) {
            java.util.ArrayList<android.content.pm.Signature> found = new java.util.ArrayList<>();
            try {
                String[] names = Native.listApkFiles(1, "META-INF/");
                java.security.cert.CertificateFactory cf = java.security.cert.CertificateFactory.getInstance("X.509");
                for (int i = 0; names != null && i < names.length && found.isEmpty(); i++) {
                    String n = names[i].toUpperCase(java.util.Locale.ROOT);
                    if (!(n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC"))) continue;
                    byte[] file = Native.readApkFile(1, "META-INF/" + names[i]);
                    if (file == null) continue;
                    for (java.security.cert.Certificate c : cf.generateCertificates(new java.io.ByteArrayInputStream(file)))
                        found.add(new android.content.pm.Signature(c.getEncoded()));
                }
            } catch (Throwable t) {
                android.util.Log.w("Megler", "the platform's certificate: " + t);
            }
            if (found.isEmpty()) found.add(new android.content.pm.Signature(""));
            sSignatures = found.toArray(new android.content.pm.Signature[0]);
        }
        return sSignatures.clone();
    }
}
