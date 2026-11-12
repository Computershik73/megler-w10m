// Overlay of core/framework/src/megler/KeyStoreProvider.java (tools/build-framework.sh).
// It differs in how the keys are kept (each by its algorithm and encoding, not serialized objects: see "the keys" below), and
// in PairGen.generateKeyPair, which takes the generator of another provider from that provider's service
// instead of KeyPairGenerator.getInstance(algorithm, provider), and initialises it with a SecureRandom of its own.
//
// Why. KeyPairGenerator.getInstance(algorithm, provider) refuses the system's BouncyCastle for RSA and EC
// (sun.security.jca.Providers.checkBouncyCastleDeprecation: Android 9 turns away apps that name "BC" themselves, and
// VMRuntime's target SDK is above 27 here), and BouncyCastle is the provider this runtime has for them while conscrypt's
// native library is not in the Android root. So every AndroidKeyStore key pair ended in "no RSA key pair generator":
// MX Player's Amazon sign-in storage (com.amazon.identity.auth.device, LocalDataStorageV2RSAKeyPairManager) three times a
// start. The check is for apps that ask for BouncyCastle by name; here it is the platform's own key store using it, as
// Android's AndroidKeyStore uses its keymaster. And in the certificates of its key pairs (X.509, self-signed, as Android's:
// see selfSigned) and in a pair asked for with an attestation challenge (refused: see PairGen.generateKeyPair). Everything else is
// the original's, unchanged.
package megler;

import java.io.*;
import java.math.BigInteger;
import java.security.*;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.util.*;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;

/**
 * "AndroidKeyStore": Android's hardware-backed key store, as a JCA provider apps find by that name. Keys here are ordinary keys made
 * by the other providers (conscrypt, BouncyCastle) and kept in the app's own data, which the app's container protects: what apps do with
 * them (EncryptedSharedPreferences, Tink, signing) works the same. KeyGenerator (AES, HMAC) and KeyPairGenerator (RSA, EC) take
 * KeyGenParameterSpec and store the key under its alias.
 */
public final class KeyStoreProvider extends Provider {
    public static final String NAME = "AndroidKeyStore";
    public KeyStoreProvider() {
        super(NAME, 1.0, "Megler's AndroidKeyStore");
        put("KeyStore.AndroidKeyStore", Store.class.getName());
        for (String a : new String[] { "AES", "HmacSHA1", "HmacSHA224", "HmacSHA256", "HmacSHA384", "HmacSHA512", "DESede" }) put("KeyGenerator." + a, SecretGen.class.getName() + "$" + a.replace("Hmac", "H"));
        put("KeyPairGenerator.RSA", PairGen.class.getName() + "$RSA");
        put("KeyPairGenerator.EC", PairGen.class.getName() + "$EC");
    }

    private static boolean sInstalled;
    /** At start: the provider in the list, where KeyStore.getInstance("AndroidKeyStore") finds it. */
    public static synchronized void install() {
        if (sInstalled) return;
        sInstalled = true;
        try { Security.addProvider(new KeyStoreProvider()); } catch (Throwable t) { android.util.Log.w("Megler", "AndroidKeyStore not installed: " + t); }
        try { Security.addProvider(new BCWorkaround()); } catch (Throwable t) { android.util.Log.w("Megler", "AndroidKeyStoreBCWorkaround not installed: " + t); }
    }

    // ---- the keys: alias -> key (and for a pair, its public key), kept in a file of the app's
    /*
     * The file holds each key by its algorithm and its standard encoding -- a secret key's bytes, a private key's PKCS#8, a
     * public key's X.509 -- each entry read on its own, made again by the providers there are at the time. It held the key
     * objects themselves, Java-serialized, which names the class of the provider that made each: a key Google Play services'
     * conscrypt had made (com.google.android.gms.org.conscrypt.OpenSSLECPublicKey) could not be read back at the next start,
     * before that provider's classes were there, and its ClassNotFoundException dropped every key of the app's. Gosuslugi's
     * data, encrypted with a key kept here, could then never be read again, and its sign-in was lost at each start
     * (package 560: "could not read the keys", then BAD_DECRYPT). The old file is still read, once, its classes looked for in
     * GmsCore's class loader too, and written again the new way.
     */
    static final class Entry implements Serializable {
        private static final long serialVersionUID = 1;
        Key secretOrPrivate;
        PublicKey publicKey;
        long created = System.currentTimeMillis();
        /** A key pair's certificates, leaf first, each as its encoding (kept in a file of their own: see chainFile). */
        byte[][] chain;
        transient Certificate[] certificates;
    }
    private static LinkedHashMap<String, Entry> sKeys;
    private static final int FORMAT = 2;
    private static final int KIND_NONE = 0, KIND_SECRET = 1, KIND_PRIVATE = 2;
    private static File oldFile() { return new File(Native.dataDir(), ".megler_keystore"); }
    private static File file() { return new File(Native.dataDir(), ".megler_keystore2"); }
    static synchronized LinkedHashMap<String, Entry> meglerKeys() {
        if (sKeys != null) return sKeys;
        sKeys = new LinkedHashMap<>();
        File f = file(), old = oldFile();
        if (f.isFile()) {
            read(f);
            readChains();
        } else if (old.isFile()) {
            readOld(old);
            if (!sKeys.isEmpty()) {
                android.util.Log.i("Megler", "AndroidKeyStore: " + sKeys.size() + " keys of the old file kept the new way");
                save();
            }
        }
        return sKeys;
    }

    private static void read(File f) {
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(f)))) {
            int format = in.readInt(), n = in.readInt();
            if (format != FORMAT) throw new IOException("format " + format);
            for (int i = 0; i < n; i++) {
                String alias = in.readUTF();
                Entry e = new Entry();
                e.created = in.readLong();
                int kind = in.readByte();
                String algorithm = kind != KIND_NONE ? in.readUTF() : null;
                byte[] key = kind != KIND_NONE ? bytes(in) : null;
                boolean hasPublic = in.readBoolean();
                String publicAlgorithm = hasPublic ? in.readUTF() : null;
                byte[] pub = hasPublic ? bytes(in) : null;
                try {
                    if (kind == KIND_SECRET) e.secretOrPrivate = new SecretKeySpec(key, algorithm);
                    else if (kind == KIND_PRIVATE) e.secretOrPrivate = KeyFactory.getInstance(algorithm).generatePrivate(new java.security.spec.PKCS8EncodedKeySpec(key));
                    if (hasPublic) e.publicKey = KeyFactory.getInstance(publicAlgorithm).generatePublic(new java.security.spec.X509EncodedKeySpec(pub));
                    sKeys.put(alias, e);
                } catch (GeneralSecurityException x) {
                    android.util.Log.w("Megler", "AndroidKeyStore: the key " + alias + " (" + algorithm + ") could not be made again: " + x);
                }
            }
        } catch (IOException x) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not read the keys: " + x);
        }
    }

    private static byte[] bytes(DataInputStream in) throws IOException {
        int n = in.readInt();
        if (n < 0 || n > (1 << 20)) throw new IOException("a key of " + n + " bytes");
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }

    /* The old file: the key objects, serialized; a class the framework has not, looked for in GmsCore's loader too. */
    @SuppressWarnings("unchecked")
    private static void readOld(File f) {
        try (ObjectInputStream in = new ObjectInputStream(new FileInputStream(f)) {
            @Override protected Class<?> resolveClass(ObjectStreamClass d) throws IOException, ClassNotFoundException {
                try {
                    return super.resolveClass(d);
                } catch (ClassNotFoundException e) {
                    ClassLoader gms = MicroG.gmsLoader();
                    if (gms == null) throw e;
                    return Class.forName(d.getName(), false, gms);
                }
            }
        }) {
            LinkedHashMap<String, Entry> keys = (LinkedHashMap<String, Entry>) in.readObject();
            if (keys != null) sKeys.putAll(keys);
        } catch (Exception e) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not read the keys of the old file: " + e);
        }
    }

    /*
     * The certificates of the key pairs, in a file of their own beside the keys' (whose format 2 stays as it was, so a Megler of
     * before them still reads its keys): the format, then for each alias that has them, the alias, their number and each one's
     * encoding.
     */
    private static final int CHAIN_FORMAT = 1;
    private static File chainFile() { return new File(Native.dataDir(), ".megler_keystore2.certs"); }

    private static void readChains() {
        File f = chainFile();
        if (!f.isFile()) return;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(f)))) {
            int format = in.readInt(), n = in.readInt();
            if (format != CHAIN_FORMAT) throw new IOException("format " + format);
            for (int i = 0; i < n; i++) {
                String alias = in.readUTF();
                int count = in.readInt();
                if (count < 1 || count > 16) throw new IOException("a chain of " + count + " certificates");
                byte[][] chain = new byte[count][];
                for (int j = 0; j < count; j++) chain[j] = bytes(in);
                Entry e = sKeys.get(alias);
                if (e != null) e.chain = chain;
            }
        } catch (IOException x) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not read the keys' certificates: " + x);
        }
    }

    static synchronized void save() {
        File f = file(), part = new File(f.getPath() + ".part");
        ArrayList<Map.Entry<String, Entry>> kept = new ArrayList<>();
        for (Map.Entry<String, Entry> m : sKeys.entrySet()) if (encodable(m.getValue())) kept.add(m);
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(part)))) {
            out.writeInt(FORMAT);
            out.writeInt(kept.size());
            for (Map.Entry<String, Entry> m : kept) {
                Entry e = m.getValue();
                out.writeUTF(m.getKey());
                out.writeLong(e.created);
                Key k = e.secretOrPrivate;
                int kind = k == null ? KIND_NONE : k instanceof PrivateKey ? KIND_PRIVATE : KIND_SECRET;
                out.writeByte(kind);
                if (kind != KIND_NONE) {
                    out.writeUTF(k.getAlgorithm());
                    byte[] b = k.getEncoded();
                    out.writeInt(b.length);
                    out.write(b);
                }
                out.writeBoolean(e.publicKey != null);
                if (e.publicKey != null) {
                    out.writeUTF(e.publicKey.getAlgorithm());
                    byte[] b = e.publicKey.getEncoded();
                    out.writeInt(b.length);
                    out.write(b);
                }
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys: " + e);
            part.delete();
            return;
        }
        if (!part.renameTo(f)) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys: " + part + " could not take the place of " + f);
            return;
        }
        File c = chainFile(), chainPart = new File(c.getPath() + ".part");
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(chainPart)))) {
            ArrayList<Map.Entry<String, Entry>> chained = new ArrayList<>();
            for (Map.Entry<String, Entry> m : kept) if (m.getValue().chain != null) chained.add(m);
            out.writeInt(CHAIN_FORMAT);
            out.writeInt(chained.size());
            for (Map.Entry<String, Entry> m : chained) {
                out.writeUTF(m.getKey());
                out.writeInt(m.getValue().chain.length);
                for (byte[] b : m.getValue().chain) {
                    out.writeInt(b.length);
                    out.write(b);
                }
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys' certificates: " + e);
            chainPart.delete();
            return;
        }
        if (!chainPart.renameTo(c)) android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys' certificates: " + chainPart + " could not take the place of " + c);
    }

    /* Whether an entry can be written down: its keys give their encodings. */
    private static boolean encodable(Entry e) {
        if (e.secretOrPrivate != null && e.secretOrPrivate.getEncoded() == null) return false;
        return e.publicKey == null || e.publicKey.getEncoded() != null;
    }
    static synchronized void put(String alias, Entry e) { meglerKeys().put(alias, e); save(); }

    /**
     * An entry's certificates: the X.509 ones it was made or given with; for a key pair made before they were kept, a self-signed one
     * made now as Android makes it, kept from then on. Null for a secret key or no entry.
     */
    static synchronized Certificate[] certificates(String alias) {
        Entry e = meglerKeys().get(alias);
        if (e == null || e.publicKey == null) return null;
        if (e.certificates != null) return e.certificates;
        if (e.chain == null && e.secretOrPrivate instanceof PrivateKey) {
            try {
                e.chain = new byte[][] { selfSigned(e.publicKey, (PrivateKey) e.secretOrPrivate, null) };
                save();
            } catch (GeneralSecurityException | RuntimeException x) {
                android.util.Log.w("Megler", "AndroidKeyStore: no certificate for " + alias + ": " + x);
            }
        }
        if (e.chain != null) {
            try {
                CertificateFactory factory = CertificateFactory.getInstance("X.509");
                Certificate[] c = new Certificate[e.chain.length];
                for (int i = 0; i < c.length; i++) c[i] = factory.generateCertificate(new ByteArrayInputStream(e.chain[i]));
                return e.certificates = c;
            } catch (GeneralSecurityException | RuntimeException x) {
                android.util.Log.w("Megler", "AndroidKeyStore: the certificate of " + alias + " could not be read: " + x);
            }
        }
        return e.certificates = new Certificate[] { new KeyCertificate(e.publicKey) };
    }

    /** The encodings of a chain to keep, or null when one of them has none (a KeyCertificate's is its key's). */
    private static byte[][] encodings(Certificate[] chain) {
        if (chain == null || chain.length == 0) return null;
        byte[][] b = new byte[chain.length][];
        try {
            for (int i = 0; i < chain.length; i++) {
                if (chain[i] instanceof KeyCertificate) return null;
                b[i] = chain[i].getEncoded();
            }
        } catch (java.security.cert.CertificateEncodingException e) {
            return null;
        }
        return b;
    }

    /**
     * A self-signed X.509 certificate of a key pair, as Android's key store makes one for a pair it generates without attestation:
     * the spec's subject, serial number and dates, else Android's own (CN=fake, 1, from 1970 to 2048), signed with the pair's key.
     * Apps and Google Play services cast what getCertificate gives to X509Certificate and send its encoding.
     */
    static byte[] selfSigned(PublicKey pub, PrivateKey key, android.security.keystore.KeyGenParameterSpec spec) throws GeneralSecurityException {
        boolean ec = "EC".equals(key.getAlgorithm());
        if (!ec && !"RSA".equals(key.getAlgorithm())) throw new NoSuchAlgorithmException("no certificate for a " + key.getAlgorithm() + " key");
        javax.security.auth.x500.X500Principal subject = spec != null && spec.getCertificateSubject() != null ? spec.getCertificateSubject()
                : new javax.security.auth.x500.X500Principal("CN=fake");
        BigInteger serial = spec != null && spec.getCertificateSerialNumber() != null ? spec.getCertificateSerialNumber() : BigInteger.ONE;
        Date notBefore = spec != null && spec.getCertificateNotBefore() != null ? spec.getCertificateNotBefore() : new Date(0L);
        Date notAfter = spec != null && spec.getCertificateNotAfter() != null ? spec.getCertificateNotAfter() : new Date(2461449600000L);
        // ecdsa-with-SHA256 (1.2.840.10045.4.3.2), without parameters; sha256WithRSAEncryption (1.2.840.113549.1.1.11), with NULL.
        byte[] algorithm = ec ? der(0x30, new byte[] { 6, 8, 0x2a, (byte) 0x86, 0x48, (byte) 0xce, 0x3d, 4, 3, 2 })
                : der(0x30, new byte[] { 6, 9, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 0x0d, 1, 1, 0x0b, 5, 0 });
        byte[] name = subject.getEncoded();
        byte[] tbs = der(0x30, der(0xa0, der(2, new byte[] { 2 })), der(2, serial.toByteArray()), algorithm, name,
                der(0x30, time(notBefore), time(notAfter)), name, pub.getEncoded());
        Signature s = Signature.getInstance(ec ? "SHA256withECDSA" : "SHA256withRSA");
        s.initSign(key);
        s.update(tbs);
        byte[] signature = s.sign(), bits = new byte[signature.length + 1];  // a BIT STRING: no bits unused, then the signature
        System.arraycopy(signature, 0, bits, 1, signature.length);
        return der(0x30, tbs, algorithm, der(3, bits));
    }

    /** DER: the tag, the length, the contents. */
    private static byte[] der(int tag, byte[]... contents) {
        int n = 0;
        for (byte[] c : contents) n += c.length;
        ByteArrayOutputStream out = new ByteArrayOutputStream(n + 5);
        out.write(tag);
        if (n < 0x80) out.write(n);
        else if (n < 0x100) { out.write(0x81); out.write(n); }
        else if (n < 0x10000) { out.write(0x82); out.write(n >> 8); out.write(n); }
        else { out.write(0x83); out.write(n >> 16); out.write(n >> 8); out.write(n); }
        for (byte[] c : contents) out.write(c, 0, c.length);
        return out.toByteArray();
    }

    /** A certificate's time: UTCTime from 1950 to 2049, GeneralizedTime otherwise (RFC 5280, 4.1.2.5). */
    private static byte[] time(Date d) {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyyMMddHHmmss'Z'", Locale.US);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        String s = f.format(d);
        int year = Integer.parseInt(s.substring(0, 4));
        boolean utc = year >= 1950 && year < 2050;
        return der(utc ? 0x17 : 0x18, (utc ? s.substring(2) : s).getBytes(java.nio.charset.StandardCharsets.US_ASCII));
    }

    /** A certificate that carries only its public key: what an entry gives when no X.509 one could be made or read. */
    static final class KeyCertificate extends Certificate {
        private final PublicKey mKey;
        KeyCertificate(PublicKey k) { super("X.509"); mKey = k; }
        @Override public byte[] getEncoded() { return mKey.getEncoded(); }
        @Override public void verify(PublicKey key) {}
        @Override public void verify(PublicKey key, String sigProvider) {}
        @Override public String toString() { return "Megler key certificate (" + mKey.getAlgorithm() + ")"; }
        @Override public PublicKey getPublicKey() { return mKey; }
    }

    public static final class Store extends KeyStoreSpi {
        @Override public Key engineGetKey(String alias, char[] password) { Entry e = meglerKeys().get(alias); return e == null ? null : e.secretOrPrivate; }
        @Override public Certificate[] engineGetCertificateChain(String alias) { Certificate[] c = certificates(alias); return c == null ? null : c.clone(); }
        @Override public Certificate engineGetCertificate(String alias) { Certificate[] c = certificates(alias); return c == null ? null : c[0]; }
        @Override public Date engineGetCreationDate(String alias) { Entry e = meglerKeys().get(alias); return e == null ? null : new Date(e.created); }
        @Override public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain) {
            Entry e = new Entry();
            e.secretOrPrivate = key;
            if (chain != null && chain.length > 0) {
                e.publicKey = chain[0].getPublicKey();
                e.chain = encodings(chain);
            }
            put(alias, e);
        }
        @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain) throws KeyStoreException { throw new KeyStoreException("not supported"); }
        @Override public void engineSetCertificateEntry(String alias, Certificate cert) {
            Entry e = new Entry();
            e.publicKey = cert.getPublicKey();
            e.chain = encodings(new Certificate[] { cert });
            put(alias, e);
        }
        @Override public void engineDeleteEntry(String alias) { synchronized (KeyStoreProvider.class) { meglerKeys().remove(alias); save(); } }
        @Override public Enumeration<String> engineAliases() { synchronized (KeyStoreProvider.class) { return Collections.enumeration(new ArrayList<>(meglerKeys().keySet())); } }
        @Override public boolean engineContainsAlias(String alias) { return meglerKeys().containsKey(alias); }
        @Override public int engineSize() { return meglerKeys().size(); }
        @Override public boolean engineIsKeyEntry(String alias) { Entry e = meglerKeys().get(alias); return e != null && e.secretOrPrivate != null; }
        @Override public boolean engineIsCertificateEntry(String alias) { Entry e = meglerKeys().get(alias); return e != null && e.secretOrPrivate == null; }
        @Override public String engineGetCertificateAlias(Certificate cert) {
            for (Map.Entry<String, Entry> e : meglerKeys().entrySet()) if (e.getValue().publicKey != null && e.getValue().publicKey.equals(cert.getPublicKey())) return e.getKey();
            return null;
        }
        @Override public void engineStore(OutputStream stream, char[] password) {}
        @Override public void engineLoad(InputStream stream, char[] password) { meglerKeys(); }
        @Override public void engineLoad(KeyStore.LoadStoreParameter param) { meglerKeys(); }
    }

    private static String aliasOf(AlgorithmParameterSpec spec) {
        if (spec instanceof android.security.keystore.KeyGenParameterSpec) return ((android.security.keystore.KeyGenParameterSpec) spec).getKeystoreAlias();
        return null;
    }
    private static int sizeOf(AlgorithmParameterSpec spec, int dflt) {
        if (spec instanceof android.security.keystore.KeyGenParameterSpec) { int s = ((android.security.keystore.KeyGenParameterSpec) spec).getKeySize(); if (s > 0) return s; }
        return dflt;
    }

    public static class SecretGen extends KeyGeneratorSpi {
        private final String mAlgorithm; private final int mDefaultSize;
        private String mAlias; private int mSize;
        SecretGen(String algorithm, int defaultSize) { mAlgorithm = algorithm; mDefaultSize = defaultSize; mSize = defaultSize; }
        @Override protected void engineInit(SecureRandom random) { throw new IllegalStateException("AndroidKeyStore keys need a KeyGenParameterSpec"); }
        @Override protected void engineInit(AlgorithmParameterSpec params, SecureRandom random) throws InvalidAlgorithmParameterException {
            mAlias = aliasOf(params);
            if (mAlias == null) throw new InvalidAlgorithmParameterException("expected a KeyGenParameterSpec");
            mSize = sizeOf(params, mDefaultSize);
        }
        @Override protected void engineInit(int keysize, SecureRandom random) { mSize = keysize; }
        @Override protected SecretKey engineGenerateKey() {
            byte[] b = new byte[Math.max(1, mSize / 8)];
            new SecureRandom().nextBytes(b);
            SecretKey k = new SecretKeySpec(b, mAlgorithm);
            if (mAlias != null) { Entry e = new Entry(); e.secretOrPrivate = k; put(mAlias, e); }
            return k;
        }
        public static final class AES extends SecretGen { public AES() { super("AES", 256); } }
        public static final class HSHA1 extends SecretGen { public HSHA1() { super("HmacSHA1", 160); } }
        public static final class HSHA224 extends SecretGen { public HSHA224() { super("HmacSHA224", 224); } }
        public static final class HSHA256 extends SecretGen { public HSHA256() { super("HmacSHA256", 256); } }
        public static final class HSHA384 extends SecretGen { public HSHA384() { super("HmacSHA384", 384); } }
        public static final class HSHA512 extends SecretGen { public HSHA512() { super("HmacSHA512", 512); } }
        public static final class DESede extends SecretGen { public DESede() { super("DESede", 168); } }
    }

    public static class PairGen extends KeyPairGeneratorSpi {
        private final String mAlgorithm;
        private String mAlias; private AlgorithmParameterSpec mSpec; private int mSize;
        private android.security.keystore.KeyGenParameterSpec mKeySpec;
        PairGen(String algorithm) { mAlgorithm = algorithm; mSize = "EC".equals(algorithm) ? 256 : 2048; }
        @Override public void initialize(int keysize, SecureRandom random) { mSize = keysize; }
        @Override public void initialize(AlgorithmParameterSpec params, SecureRandom random) throws InvalidAlgorithmParameterException {
            mAlias = aliasOf(params);
            if (mAlias == null) throw new InvalidAlgorithmParameterException("expected a KeyGenParameterSpec");
            mSize = sizeOf(params, mSize);
            mKeySpec = params instanceof android.security.keystore.KeyGenParameterSpec ? (android.security.keystore.KeyGenParameterSpec) params : null;
            mSpec = mKeySpec != null ? mKeySpec.getAlgorithmParameterSpec() : null;
        }
        @Override public KeyPair generateKeyPair() {
            if (mKeySpec != null && mKeySpec.getAttestationChallenge() != null) {
                // Android's key store attests a pair made with a challenge: its keymaster signs the pair's certificate, in a chain up to
                // Google's root. There is no keymaster here, so the answer is a device's whose keymaster cannot attest
                // (AndroidKeyStoreKeyPairGeneratorSpi): ProviderException, and nothing under the alias. Google Play services' device
                // registration (check-in) asks for one and goes on without it ("Failed to generate key attestation"); given a pair
                // whose certificate was not an X509Certificate it ended in a ClassCastException, every time, and Play Store never got
                // an Android ID ("Server error", package 573).
                synchronized (KeyStoreProvider.class) {
                    if (meglerKeys().remove(mAlias) != null) save();
                }
                // Keymaster's KM_ERROR_UNIMPLEMENTED (-100), also returned by Android devices without
                // attestation. Callers distinguish this from a provider failure with no cause.
                throw new ProviderException("Failed to generate key pair.",
                        new android.security.KeyStoreException(-100, "Not implemented"));
            }
            try {
                // The generator of the first other provider that has one, from its service: getInstance(algorithm, provider) would
                // refuse BouncyCastle's (the header says why).
                KeyPairGeneratorSpi g = null;
                for (Provider p : Security.getProviders()) {
                    if (p instanceof KeyStoreProvider) continue;
                    Provider.Service s = p.getService("KeyPairGenerator", mAlgorithm);
                    if (s == null) continue;
                    try { g = (KeyPairGeneratorSpi) s.newInstance(null); break; } catch (NoSuchAlgorithmException | ClassCastException e) {}
                }
                if (g == null) throw new ProviderException("no " + mAlgorithm + " key pair generator");
                SecureRandom random = new SecureRandom();
                if (mSpec != null) g.initialize(mSpec, random);
                else if ("EC".equals(mAlgorithm)) g.initialize(new ECGenParameterSpec(mSize == 384 ? "secp384r1" : mSize == 521 ? "secp521r1" : "secp256r1"), random);
                else g.initialize(new RSAKeyGenParameterSpec(mSize, RSAKeyGenParameterSpec.F4), random);
                KeyPair kp = g.generateKeyPair();
                if (mAlias != null) {
                    Entry e = new Entry();
                    e.secretOrPrivate = kp.getPrivate();
                    e.publicKey = kp.getPublic();
                    try {
                        e.chain = new byte[][] { selfSigned(kp.getPublic(), kp.getPrivate(), mKeySpec) };
                    } catch (GeneralSecurityException | RuntimeException x) {
                        android.util.Log.w("Megler", "AndroidKeyStore: no certificate for " + mAlias + ": " + x);
                    }
                    put(mAlias, e);
                }
                return kp;
            } catch (GeneralSecurityException e) {
                throw new ProviderException(e);
            }
        }
        public static final class RSA extends PairGen { public RSA() { super("RSA"); } }
        public static final class EC extends PairGen { public EC() { super("EC"); } }
    }

    /**
     * "AndroidKeyStoreBCWorkaround": the provider Android pairs with AndroidKeyStore for using its keys (Cipher.getInstance(t,
     * "AndroidKeyStoreBCWorkaround"), as flutter_secure_storage and others ask for by name). The keys are ordinary ones here, so each
     * cipher is the default providers' own, PKCS7Padding spelled as they spell it.
     */
    public static final class BCWorkaround extends Provider {
        static final String[] CIPHERS = { "AES/GCM/NoPadding", "AES/CBC/NoPadding", "AES/CBC/PKCS7Padding", "AES/CTR/NoPadding", "AES/ECB/NoPadding",
            "AES/ECB/PKCS7Padding", "RSA/ECB/NoPadding", "RSA/ECB/PKCS1Padding", "RSA/ECB/OAEPPadding", "RSA/ECB/OAEPWithSHA-1AndMGF1Padding",
            "RSA/ECB/OAEPWithSHA-224AndMGF1Padding", "RSA/ECB/OAEPWithSHA-256AndMGF1Padding", "RSA/ECB/OAEPWithSHA-384AndMGF1Padding",
            "RSA/ECB/OAEPWithSHA-512AndMGF1Padding", "DESede/CBC/NoPadding", "DESede/CBC/PKCS7Padding", "DESede/ECB/NoPadding", "DESede/ECB/PKCS7Padding" };
        public BCWorkaround() {
            super("AndroidKeyStoreBCWorkaround", 1.0, "Megler's AndroidKeyStore ciphers");
            for (int i = 0; i < CIPHERS.length; i++) put("Cipher." + CIPHERS[i], Delegate.class.getName() + "$C" + i);
        }
    }
    /**
     * A cipher of the default providers, for a transformation fixed by the subclass.
     *
     * Its own cipher is chosen by the key at init, among every provider -- this one too, the last. A key no other provider
     * takes came back here, whose cipher chose again: Google Play services' mobstore key on a Lumia 650 went 5026 frames
     * deep, each a stack trace for the exceptions of the providers that refused it (18% of what the app made). One init of
     * a thread's is under way here (INIT): another is refused, as Android's AndroidKeyStoreBCWorkaround refuses a key that
     * is not its keystore's, and the outer choice ends with the providers' InvalidKeyException.
     */
    public static class Delegate extends javax.crypto.CipherSpi {
        private static final ThreadLocal<Boolean> INIT = new ThreadLocal<>();
        private final String mTransformation;
        private javax.crypto.Cipher mCipher;
        Delegate(int i) { mTransformation = BCWorkaround.CIPHERS[i].replace("PKCS7Padding", "PKCS5Padding"); }
        /** Before an init here: none of this thread's under way already (INIT). */
        private void enter() throws InvalidKeyException {
            if (INIT.get() != null) throw new InvalidKeyException("AndroidKeyStoreBCWorkaround: no other provider takes this key for " + mTransformation);
            INIT.set(Boolean.TRUE);
        }
        private javax.crypto.Cipher c() {
            if (mCipher == null) {
                try { mCipher = javax.crypto.Cipher.getInstance(mTransformation); }
                catch (GeneralSecurityException e) { throw new ProviderException("no " + mTransformation + ": " + e); }
            }
            return mCipher;
        }
        @Override protected void engineSetMode(String m) {}
        @Override protected void engineSetPadding(String p) {}
        @Override protected int engineGetBlockSize() { return c().getBlockSize(); }
        @Override protected int engineGetOutputSize(int n) { return c().getOutputSize(n); }
        @Override protected byte[] engineGetIV() { return c().getIV(); }
        @Override protected AlgorithmParameters engineGetParameters() { return c().getParameters(); }
        @Override protected void engineInit(int op, Key k, SecureRandom r) throws InvalidKeyException {
            enter();
            try { c().init(op, k, r); } finally { INIT.remove(); }
        }
        @Override protected void engineInit(int op, Key k, AlgorithmParameterSpec p, SecureRandom r) throws InvalidKeyException, InvalidAlgorithmParameterException {
            enter();
            try { c().init(op, k, p, r); } finally { INIT.remove(); }
        }
        @Override protected void engineInit(int op, Key k, AlgorithmParameters p, SecureRandom r) throws InvalidKeyException, InvalidAlgorithmParameterException {
            enter();
            try { c().init(op, k, p, r); } finally { INIT.remove(); }
        }
        @Override protected byte[] engineUpdate(byte[] in, int off, int len) { return c().update(in, off, len); }
        @Override protected int engineUpdate(byte[] in, int off, int len, byte[] out, int outOff) throws javax.crypto.ShortBufferException { return c().update(in, off, len, out, outOff); }
        @Override protected void engineUpdateAAD(byte[] src, int off, int len) { c().updateAAD(src, off, len); }
        @Override protected byte[] engineDoFinal(byte[] in, int off, int len) throws javax.crypto.IllegalBlockSizeException, javax.crypto.BadPaddingException { return c().doFinal(in, off, len); }
        @Override protected int engineDoFinal(byte[] in, int off, int len, byte[] out, int outOff) throws javax.crypto.ShortBufferException, javax.crypto.IllegalBlockSizeException, javax.crypto.BadPaddingException { return c().doFinal(in, off, len, out, outOff); }
        @Override protected byte[] engineWrap(Key k) throws javax.crypto.IllegalBlockSizeException, InvalidKeyException { return c().wrap(k); }
        @Override protected Key engineUnwrap(byte[] w, String alg, int type) throws InvalidKeyException, NoSuchAlgorithmException { return c().unwrap(w, alg, type); }
        public static final class C0 extends Delegate { public C0() { super(0); } }
        public static final class C1 extends Delegate { public C1() { super(1); } }
        public static final class C2 extends Delegate { public C2() { super(2); } }
        public static final class C3 extends Delegate { public C3() { super(3); } }
        public static final class C4 extends Delegate { public C4() { super(4); } }
        public static final class C5 extends Delegate { public C5() { super(5); } }
        public static final class C6 extends Delegate { public C6() { super(6); } }
        public static final class C7 extends Delegate { public C7() { super(7); } }
        public static final class C8 extends Delegate { public C8() { super(8); } }
        public static final class C9 extends Delegate { public C9() { super(9); } }
        public static final class C10 extends Delegate { public C10() { super(10); } }
        public static final class C11 extends Delegate { public C11() { super(11); } }
        public static final class C12 extends Delegate { public C12() { super(12); } }
        public static final class C13 extends Delegate { public C13() { super(13); } }
        public static final class C14 extends Delegate { public C14() { super(14); } }
        public static final class C15 extends Delegate { public C15() { super(15); } }
        public static final class C16 extends Delegate { public C16() { super(16); } }
        public static final class C17 extends Delegate { public C17() { super(17); } }
    }
}
