// Overlay of core/framework/src/megler/KeyStoreProvider.java (tools/build-framework.sh).
// It differs in how the keys are kept (each by its algorithm and encoding, not serialized objects: see "the keys" below), in
// whose they are (Google Play services' are one store in their shared data, whichever process asks: see "Whose keys"; and
// their key set whose key is in no store is removed: see lost), and in PairGen.generateKeyPair, which takes the generator of
// another provider from that provider's service instead of KeyPairGenerator.getInstance(algorithm, provider), and initialises
// it with a SecureRandom of its own.
//
// Why. KeyPairGenerator.getInstance(algorithm, provider) refuses the system's BouncyCastle for RSA and EC
// (sun.security.jca.Providers.checkBouncyCastleDeprecation: Android 9 turns away apps that name "BC" themselves, and
// VMRuntime's target SDK is above 27 here), and BouncyCastle is the provider this runtime has for them while conscrypt's
// native library is not in the Android root. So every AndroidKeyStore key pair ended in "no RSA key pair generator":
// MX Player's Amazon sign-in storage (com.amazon.identity.auth.device, LocalDataStorageV2RSAKeyPairManager) three times a
// start. The check is for apps that ask for BouncyCastle by name; here it is the platform's own key store using it, as
// Android's AndroidKeyStore uses its keymaster. And in the certificates of its key pairs (X.509, self-signed, as Android's:
// see selfSigned) and in a pair asked for with an attestation challenge (opt-in external signer: see KeyboxAttestation). Everything else is
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
 * by the other providers (conscrypt, BouncyCastle) and kept in the app's own data (Google Play services' in theirs), which the app's
 * container protects: what apps do with them (EncryptedSharedPreferences, Tink, signing) works the same. KeyGenerator (AES, HMAC)
 * and KeyPairGenerator (RSA, EC) take KeyGenParameterSpec and store the key under its alias.
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

    // ---- the keys: alias -> key (and for a pair, its public key), kept in a file: an app's in its own data, Google Play
    // services' in theirs
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
        /** A key pair's certificates, leaf first, each as its encoding (kept in a file of their own: see Box.chainFile). */
        byte[][] chain;
        transient Certificate[] certificates;
        /** The box it is in. */
        transient Box home;
    }
    private static final int FORMAT = 2;
    private static final int KIND_NONE = 0, KIND_SECRET = 1, KIND_PRIVATE = 2;

    /*
     * Whose keys (view). An app's are in a file of its own data folder. Google Play services' are in a file of their data
     * folder, the one every process that runs them has (MicroG.SHARED; the private folder of the Google Play services app
     * is that same folder): one store, whichever process asks.
     *
     * They were the keys of the app whose process asked. Google Play services keep their data in the shared folder and
     * encrypt it with keys of this store (Tink: an AES key under the alias mobstore_encrypt, and the key set it protects in
     * a file of theirs). So the process that happened to make the key set could read it and no other: "Keystore cannot load
     * the key with ID: mobstore_encrypt" and "Failed to initialize encryption" in Play Store, in Google Play services' own
     * app and in other apps (18 users' logs of 681-699; the Lumia 650's Play Store with 701).
     *
     * The keys their earlier data left in the apps' own stores are wanted until that data is cleared. The mark
     * .megler-keystore-shared beside their data folder says that there are none: run/mg_run.c writes it where they have no
     * data yet (mg_run_services_key_store: a first start, and after Clear Data). Without it a call of theirs also looks in
     * the hosting app's own store, and in the imports -- a second file of the shared folder, where each key found in an
     * app's own store and used is copied, for the other processes. Of an alias in both the newer key is the one: a key is
     * made when a key set is, so the newest belongs to the key set that is there now. With the mark the shared store is
     * the only one.
     *
     * Their code never changes the keys of an app's own store (an alias can be in both: androidx's
     * _androidx_security_master_key_). What it makes goes to the shared store. What it deletes of the app's is hidden from
     * it while the process runs (sGone); at the next start it is found again, unless another was made under the alias, as
     * the code that deletes a key does.
     */
    /** One store: a file of keys, the file of their certificates beside it, and what was read of them. */
    static final class Box {
        final File file, chainFile, lockFile;
        /** Other processes write it too: read again when the file is another, changed with the lock file held. */
        final boolean shared;
        /** An app's own: where it has no file, the one of before format 2 beside it is read, once. */
        final boolean old;
        LinkedHashMap<String, Entry> keys;
        /** The keys' file as it was when read: its time and its length (0 and 0: none). */
        long stamp = -1, length = -1;
        Box(File f, boolean shared, boolean old) {
            file = f;
            chainFile = new File(f.getPath() + ".certs");
            lockFile = new File(f.getPath() + ".lock");
            this.shared = shared;
            this.old = old;
        }
        File oldFile() { return new File(file.getParentFile(), ".megler_keystore"); }
    }
    private static final String KEYS = ".megler_keystore2", MARK = ".megler-keystore-shared";
    /* One Box for a file, whoever asks. */
    private static final HashMap<String, Box> sBoxes = new HashMap<>();
    private static Box box(File f, boolean shared, boolean old) {
        Box b = sBoxes.get(f.getPath());
        if (b == null) sBoxes.put(f.getPath(), b = new Box(f, shared, old));
        return b;
    }

    /**
     * What a call sees. `main`: where its keys are made, and looked for first. For Google Play services whose data is older
     * than the shared store, also `imports` and, in an app's process, `app`: the app's own store, which they only read.
     */
    private static final class View {
        final Box main, app, imports;
        View(Box main, Box app, Box imports) { this.main = main; this.app = app; this.imports = imports; }
    }
    private static File sServices;   // MicroG.SHARED, once it was seen to be there
    private static File sTestRoot;
    private static Thread sTestThread;
    /* The app's own aliases Google Play services deleted in this process, and those copied to the imports (alias/created). */
    private static final HashSet<String> sGone = new HashSet<>(), sExported = new HashSet<>();

    /**
     * For the tests: from now on the calling thread's key store calls are Google Play services' in an app's process, with
     * the folder `root` in the place of /data/megler-services (their data in root/com.google.android.gms, the mark
     * root/.megler-keystore-shared); null ends it.
     */
    public static synchronized void hostedForTests(String root) {
        sTestRoot = root == null ? null : new File(root);
        sTestThread = root == null ? null : Thread.currentThread();
    }

    private static View view() {
        boolean itself = false;
        File root = null;
        if (sTestRoot != null && Thread.currentThread() == sTestThread) {
            root = sTestRoot;
        } else {
            itself = GooglePackages.GMS.equals(Native.packageName());
            if (itself || MicroG.calledByHosted()) {
                if (sServices == null) {
                    File f = new File(MicroG.SHARED);
                    if (f.isDirectory()) sServices = f;
                }
                root = sServices;
            }
        }
        Box own = box(new File(Native.dataDir(), KEYS), itself, true);
        if (root == null) return new View(own, null, null);
        File data = new File(root, GooglePackages.GMS);
        Box main = itself ? own : box(new File(data, KEYS), true, false);
        if (new File(root, MARK).exists()) return new View(main, null, null);
        return new View(main, itself ? null : own, box(new File(data, KEYS + ".hosts"), true, false));
    }

    /** A box's keys: read at first, and a shared one's again when its file is not the one that was read. */
    private static LinkedHashMap<String, Entry> keys(Box b) {
        if (b.keys != null && (!b.shared || (b.file.lastModified() == b.stamp && b.file.length() == b.length))) return b.keys;
        // A shared one's file is read with the lock held: who writes puts a new file in its place, which a file held open
        // could refuse.
        boolean there = b.file.exists();
        boolean locked = b.shared && (there || (b.old && b.keys == null && b.oldFile().isFile())) && lock(b);
        try {
            load(b, locked || !b.shared || !there);
        } finally {
            if (locked) unlock(b);
        }
        return b.keys;
    }

    /*
     * A box's keys as its files have them now. `sure`: nobody writes the files meanwhile (the lock is held, or there is no
     * file), so the file's time and length stand for what was read, and it is not read again until they are others. Where
     * the file cannot be read, what was read before stays; and so do the keys that cannot be written down (encodable),
     * which are in no file.
     */
    private static void load(Box b, boolean sure) {
        long stamp = b.file.lastModified(), length = b.file.length();
        LinkedHashMap<String, Entry> now = new LinkedHashMap<>();
        boolean good = true, fromOld = false;
        if (stamp != 0 || length != 0) {
            good = read(b, now);
        } else if (b.old && b.keys == null && b.oldFile().isFile()) {
            readOld(b.oldFile(), now);
            fromOld = !now.isEmpty();
        }
        if (b.keys != null) {
            if (!good) now = b.keys;
            else for (Map.Entry<String, Entry> m : b.keys.entrySet()) if (!now.containsKey(m.getKey()) && !encodable(m.getValue())) now.put(m.getKey(), m.getValue());
        }
        for (Entry e : now.values()) e.home = b;
        b.keys = now;
        if (sure) {
            b.stamp = stamp;
            b.length = length;
        }
        if (fromOld) {
            android.util.Log.i("Megler", "AndroidKeyStore: " + now.size() + " keys of the old file kept the new way");
            save(b);
        }
    }

    /* False when the keys' file could not be read to its end (the keys before that are in `keys`). */
    private static boolean read(Box b, LinkedHashMap<String, Entry> keys) {
        boolean good = true;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(b.file)))) {
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
                    keys.put(alias, e);
                } catch (GeneralSecurityException x) {
                    android.util.Log.w("Megler", "AndroidKeyStore: the key " + alias + " (" + algorithm + ") could not be made again: " + x);
                }
            }
        } catch (IOException x) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not read the keys: " + x + " (" + b.file + ")");
            good = false;
        }
        readChains(b, keys);
        return good;
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
    private static void readOld(File f, LinkedHashMap<String, Entry> into) {
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
            if (keys != null) into.putAll(keys);
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

    private static void readChains(Box b, LinkedHashMap<String, Entry> keys) {
        if (!b.chainFile.isFile()) return;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(b.chainFile)))) {
            int format = in.readInt(), n = in.readInt();
            if (format != CHAIN_FORMAT) throw new IOException("format " + format);
            for (int i = 0; i < n; i++) {
                String alias = in.readUTF();
                int count = in.readInt();
                if (count < 1 || count > 16) throw new IOException("a chain of " + count + " certificates");
                byte[][] chain = new byte[count][];
                for (int j = 0; j < count; j++) chain[j] = bytes(in);
                Entry e = keys.get(alias);
                if (e != null) e.chain = chain;
            }
        } catch (IOException x) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not read the keys' certificates: " + x + " (" + b.chainFile + ")");
        }
    }

    /*
     * A box's keys written down: the certificates' file first and the keys' file last, each whole in a file beside it that
     * then takes its place. Another process reads both again when the keys' file is another (keys), and the certificates of
     * those keys are there by then.
     */
    private static void save(Box b) {
        ArrayList<Map.Entry<String, Entry>> kept = new ArrayList<>();
        for (Map.Entry<String, Entry> m : b.keys.entrySet()) if (encodable(m.getValue())) kept.add(m);
        File parent = b.file.getParentFile();
        if (parent != null) parent.mkdirs();
        File chainPart = new File(b.chainFile.getPath() + ".part");
        boolean chains = true;
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(chainPart)))) {
            ArrayList<Map.Entry<String, Entry>> chained = new ArrayList<>();
            for (Map.Entry<String, Entry> m : kept) if (m.getValue().chain != null) chained.add(m);
            out.writeInt(CHAIN_FORMAT);
            out.writeInt(chained.size());
            for (Map.Entry<String, Entry> m : chained) {
                out.writeUTF(m.getKey());
                out.writeInt(m.getValue().chain.length);
                for (byte[] c : m.getValue().chain) {
                    out.writeInt(c.length);
                    out.write(c);
                }
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys' certificates: " + e);
            chainPart.delete();
            chains = false;
        }
        if (chains && !chainPart.renameTo(b.chainFile)) android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys' certificates: " + chainPart + " could not take the place of " + b.chainFile);
        File part = new File(b.file.getPath() + ".part");
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
                    byte[] bytes = k.getEncoded();
                    out.writeInt(bytes.length);
                    out.write(bytes);
                }
                out.writeBoolean(e.publicKey != null);
                if (e.publicKey != null) {
                    out.writeUTF(e.publicKey.getAlgorithm());
                    byte[] bytes = e.publicKey.getEncoded();
                    out.writeInt(bytes.length);
                    out.write(bytes);
                }
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys: " + e);
            part.delete();
            return;
        }
        if (!part.renameTo(b.file)) {
            android.util.Log.w("Megler", "AndroidKeyStore: could not save the keys: " + part + " could not take the place of " + b.file);
            return;
        }
        // A shared one's time goes forward at every writing, whatever the grain of the file times: with the length as it was
        // (a key deleted, another made under its alias), the time is all that tells another process the file from the one it read.
        long stamp = b.file.lastModified();
        if (b.shared && b.stamp > 0 && stamp <= b.stamp && b.file.setLastModified(b.stamp + 1000)) stamp = b.file.lastModified();
        b.stamp = stamp;
        b.length = b.file.length();
    }

    /*
     * A shared box's lock: the file <keys' file>.lock, made by the one process that holds it and deleted when that is done
     * (a reading; or a reading, a change and a writing: milliseconds). One a process left behind when it ended, older than
     * ten seconds, is taken away. False where it cannot be had within two seconds, or cannot be made at all: the caller goes
     * on without it, as every caller did before there was one. (A process Windows suspends while it holds the lock writes
     * when it runs again, over what others wrote meanwhile: it would have to be suspended within those milliseconds.)
     */
    private static boolean lock(Box b) {
        for (long end = System.nanoTime() + 2000000000L;;) {
            try {
                if (b.lockFile.createNewFile()) return true;
            } catch (IOException e) {
                return false;
            }
            long made = b.lockFile.lastModified();
            boolean left = made != 0 && Math.abs(System.currentTimeMillis() - made) > 10000;
            if (left && b.lockFile.delete()) continue;
            if (System.nanoTime() - end >= 0) {
                android.util.Log.w("Megler", "AndroidKeyStore: " + b.lockFile + " is held by another process: going on without it");
                return false;
            }
            try {
                Thread.sleep(20);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
    }
    private static void unlock(Box b) { b.lockFile.delete(); }

    private interface Change {
        /** Whether it changed them. */
        boolean apply(LinkedHashMap<String, Entry> keys);
    }
    /** A box's keys changed, and written down when `c` says they are others: a shared one's as its files have them now, its lock held. */
    private static void change(Box b, Change c) {
        if (!b.shared) {
            if (c.apply(keys(b))) save(b);
            return;
        }
        File parent = b.file.getParentFile();
        if (parent != null) parent.mkdirs();
        boolean locked = lock(b);
        try {
            load(b, locked);
            if (c.apply(b.keys)) save(b);
        } finally {
            if (locked) unlock(b);
        }
    }

    /* Whether an entry can be written down: its keys give their encodings. */
    private static boolean encodable(Entry e) {
        if (e.secretOrPrivate != null && e.secretOrPrivate.getEncoded() == null) return false;
        return e.publicKey == null || e.publicKey.getEncoded() != null;
    }

    /*
     * The entry under an alias, as the view has it: its main store's; else (Google Play services with data older than the
     * shared store) the newer of the app's own and the imports'. `taken`: the caller gets the key itself, so one found in
     * the app's own store goes to the imports (export).
     */
    private static Entry find(View v, String alias, boolean taken) {
        if (alias == null) return null;
        Entry e = keys(v.main).get(alias);
        if (e != null || v.imports == null) return e;
        Entry own = v.app != null && !sGone.contains(alias) ? keys(v.app).get(alias) : null;
        Entry imported = keys(v.imports).get(alias);
        if (own == null || (imported != null && imported.created >= own.created)) return imported;
        if (taken) export(v, alias, own);
        return own;
    }

    /* A key of the app's own store that Google Play services use, copied to the imports for the other processes: once in a process, and where the imports have none as new. */
    private static void export(View v, String alias, Entry e) {
        if (!sExported.add(alias + "/" + e.created) || !encodable(e)) return;
        Box imports = v.imports;
        change(imports, keys -> {
            Entry there = keys.get(alias);
            if (there != null && there.created >= e.created) return false;
            Entry copy = new Entry();
            copy.secretOrPrivate = e.secretOrPrivate;
            copy.publicKey = e.publicKey;
            copy.created = e.created;
            copy.chain = e.chain;
            copy.home = imports;
            keys.put(alias, copy);
            return true;
        });
    }

    static synchronized Entry entry(String alias, boolean taken) {
        View v = view();
        Entry e = find(v, alias, taken);
        if (e == null && taken && v.main.shared) lost(v.main, alias);
        return e;
    }

    /*
     * A key Google Play services take (getKey) that is in no store they read, and the Tink key set it encrypted, kept in their
     * preferences under the key's alias as their mobstore keeps it (shared_prefs/mobstore_encrypt.xml): the key set is
     * removed, so that they make the key and the key set again, as they do on a phone where neither is there yet.
     *
     * Android has no such state: an app's keys go only with its data. Megler before 0.1.0.734 kept their keys in the store of
     * whichever app's process made them; the key set they made so in their shared data is unreadable everywhere else, and
     * for good where that app is gone, was cleared, or never runs a Megler that copies its key to the imports (an app's
     * package of an older Megler). Every app hosting them then failed at each use of their storage: "Keystore cannot load the
     * key with ID: mobstore_encrypt", then "Failed to initialize encryption" (issue #21: the GitHub app and Via on a PC; seven
     * times a run in each of five apps on a Lumia 650 with 734). Tink reads the key set first and asks for the key after, so
     * that use still fails; the next one starts afresh. What the old key set encrypted cannot be read with or without it.
     * Once a process for an alias; nothing is removed where there is no such file, and an alias that is no file name is left.
     */
    private static final HashSet<String> sLost = new HashSet<>();
    private static void lost(Box main, String alias) {
        if (!alias.matches("[A-Za-z0-9_][A-Za-z0-9_.-]*") || !sLost.add(alias)) return;
        File keySet = new File(new File(main.file.getParentFile(), "shared_prefs"), alias + ".xml");
        if (!keySet.isFile()) return;
        if (keySet.delete())
            android.util.Log.w("Megler", "AndroidKeyStore: Google Play services' key " + alias + " is in no store they read (made in another app's process"
                    + " by a Megler before 0.1.0.734): its key set " + keySet + " is removed, and they make both again");
        else android.util.Log.w("Megler", "AndroidKeyStore: Google Play services' key " + alias + " is in no store they read, and its key set " + keySet + " could not be removed");
    }

    private static ArrayList<String> aliases(View v) {
        LinkedHashSet<String> all = new LinkedHashSet<>(keys(v.main).keySet());
        if (v.imports != null) {
            if (v.app != null) for (String a : keys(v.app).keySet()) if (!sGone.contains(a)) all.add(a);
            all.addAll(keys(v.imports).keySet());
        }
        return new ArrayList<>(all);
    }
    static synchronized ArrayList<String> aliases() { return aliases(view()); }

    /** The store read, where it was not yet (KeyStore.load). */
    static synchronized void touch() { keys(view().main); }

    static synchronized void put(String alias, Entry e) {
        Box main = view().main;
        e.home = main;
        change(main, keys -> {
            keys.put(alias, e);
            return true;
        });
    }

    /** What is under an alias gone, as the view has it (the app's own, for Google Play services: hidden); whether there was anything. */
    static synchronized boolean remove(String alias) {
        if (alias == null) return false;
        View v = view();
        boolean had = drop(v.main, alias);
        if (v.imports != null) {
            if (drop(v.imports, alias)) had = true;
            if (v.app != null && keys(v.app).containsKey(alias) && sGone.add(alias)) had = true;
        }
        return had;
    }
    private static boolean drop(Box b, String alias) {
        if (!keys(b).containsKey(alias)) return false;
        change(b, keys -> keys.remove(alias) != null);
        return true;
    }

    /**
     * An entry's certificates: the X.509 ones it was made or given with; for a key pair made before they were kept, a self-signed one
     * made now as Android makes it, kept from then on in the store the entry is in. Null for a secret key or no entry.
     */
    static synchronized Certificate[] certificates(String alias) {
        Entry e = find(view(), alias, true);
        if (e == null || e.publicKey == null) return null;
        if (e.certificates != null) return e.certificates;
        if (e.chain == null && e.secretOrPrivate instanceof PrivateKey) {
            try {
                byte[][] made = new byte[][] { selfSigned(e.publicKey, (PrivateKey) e.secretOrPrivate, null) };
                byte[][][] kept = { made };
                byte[] key = e.publicKey.getEncoded();
                if (e.home != null && key != null) change(e.home, keys -> {
                    // (A shared store was read again: the entry is another object, and may be another key or have a certificate by now.)
                    Entry now = keys.get(alias);
                    if (now == null || now.publicKey == null || !Arrays.equals(key, now.publicKey.getEncoded())) return false;
                    if (now.chain != null) {
                        kept[0] = now.chain;
                        return false;
                    }
                    now.chain = made;
                    return true;
                });
                e.chain = kept[0];
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
    static byte[] der(int tag, byte[]... contents) {
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
    static byte[] time(Date d) {
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
        @Override public Key engineGetKey(String alias, char[] password) { Entry e = entry(alias, true); return e == null ? null : e.secretOrPrivate; }
        @Override public Certificate[] engineGetCertificateChain(String alias) {
            if (IntegrityProfile.blocksCertificateChain()) throw new UnsupportedOperationException();
            Certificate[] c = certificates(alias);
            return c == null ? null : c.clone();
        }
        @Override public Certificate engineGetCertificate(String alias) { Certificate[] c = certificates(alias); return c == null ? null : c[0]; }
        @Override public Date engineGetCreationDate(String alias) { Entry e = entry(alias, false); return e == null ? null : new Date(e.created); }
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
        @Override public void engineDeleteEntry(String alias) { remove(alias); }
        @Override public Enumeration<String> engineAliases() { return Collections.enumeration(aliases()); }
        @Override public boolean engineContainsAlias(String alias) { return entry(alias, false) != null; }
        @Override public int engineSize() { return aliases().size(); }
        @Override public boolean engineIsKeyEntry(String alias) { Entry e = entry(alias, false); return e != null && e.secretOrPrivate != null; }
        @Override public boolean engineIsCertificateEntry(String alias) { Entry e = entry(alias, false); return e != null && e.secretOrPrivate == null; }
        @Override public String engineGetCertificateAlias(Certificate cert) {
            synchronized (KeyStoreProvider.class) {
                View v = view();
                for (String alias : aliases(v)) {
                    Entry e = find(v, alias, false);
                    if (e != null && e.publicKey != null && e.publicKey.equals(cert.getPublicKey())) return alias;
                }
            }
            return null;
        }
        @Override public void engineStore(OutputStream stream, char[] password) {}
        @Override public void engineLoad(InputStream stream, char[] password) { touch(); }
        @Override public void engineLoad(KeyStore.LoadStoreParameter param) { touch(); }
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
            boolean challenged = mKeySpec != null && mKeySpec.getAttestationChallenge() != null;
            if (challenged && Native.flag("trace-attestation"))
                android.util.Log.i("KeyboxAttestation", "Requested " + mAlgorithm + "; challenge bytes "
                        + mKeySpec.getAttestationChallenge().length + "; keybox mode " + KeyboxAttestation.enabled());
            if (challenged && !KeyboxAttestation.enabled()) {
                // Android's key store attests a pair made with a challenge: its keymaster signs the pair's certificate, in a chain up to
                // Google's root. There is no keymaster here, so the answer is a device's whose keymaster cannot attest
                // (AndroidKeyStoreKeyPairGeneratorSpi): ProviderException, and nothing under the alias. Google Play services' device
                // registration (check-in) asks for one and goes on without it ("Failed to generate key attestation"); given a pair
                // whose certificate was not an X509Certificate it ended in a ClassCastException, every time, and Play Store never got
                // an Android ID ("Server error", package 573).
                // Keep any earlier key under this alias: a failed generation must not delete it.
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
                KeyboxAttestation.Issuer issuer = challenged ? KeyboxAttestation.issuer(mKeySpec, mAlgorithm) : null;
                KeyPair kp = g.generateKeyPair();
                if (mAlias != null) {
                    Entry e = new Entry();
                    e.secretOrPrivate = kp.getPrivate();
                    e.publicKey = kp.getPublic();
                    if (issuer != null) {
                        e.chain = KeyboxAttestation.chain(kp.getPublic(), mKeySpec, issuer);
                    } else try {
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
