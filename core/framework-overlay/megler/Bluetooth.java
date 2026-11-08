// Megler: the phone's Bluetooth LE radio for android.bluetooth -- the core's (core/src/run/mg_bluetooth.c over the shell's
// megler_bluetooth_host; core/src/drivers/mg_java_bluetooth.c for these natives) -- and the framework's one thread that
// takes what the radio did and hands each event to Android's object it is for: the adapter's state to BluetoothAdapter,
// an advertisement to the app's scans (android.bluetooth.le.BluetoothLeScanner), a connection's events to its
// BluetoothGatt, a bond's to BluetoothDevice. Those hand them on to the app on Android's threads: scans' results and
// broadcasts on the main thread, a BluetoothGatt's callbacks on the Handler it was given or on a thread of the framework's,
// "BluetoothGatt", as Android's come on a binder thread.
//
// Android 12's permissions, for an app that targets it: what scans needs BLUETOOTH_SCAN, what touches a device or the
// adapter's name BLUETOOTH_CONNECT, else SecurityException, as on Android (enforce). An older app is not held to its
// manifest's BLUETOOTH and BLUETOOTH_ADMIN here, and its scans give it results without the location too, which Android 10
// and later would ask of it (BluetoothLeScanner): the location is Windows' to grant, and has nothing to do with the radio.
package megler;

import android.util.Log;

public final class Bluetooth {
    private static final String TAG = "Bluetooth";

    /* megler.h's numbers. */
    public static final int RADIO_NONE = 0, RADIO_OFF = 1, RADIO_ON = 2;
    public static final int SCAN_OFF = 0, SCAN_PASSIVE = 1, SCAN_ACTIVE = 2;
    public static final int ADDRESS_PUBLIC = 0, ADDRESS_RANDOM = 1, ADDRESS_UNKNOWN = 2;
    public static final int WRITE_COMMAND = 0, WRITE_REQUEST = 1, WRITE_SIGNED = 2;
    public static final int PAIR_REFUSE = 0, PAIR_ACCEPT = 1, PAIR_ASK_USER = 2;
    public static final int ATTR_SERVICE = 1, ATTR_SECONDARY = 2, ATTR_INCLUDED = 3, ATTR_CHARACTERISTIC = 4, ATTR_DESCRIPTOR = 5;
    public static final int EVENT_STATE = 1, EVENT_ADVERTISEMENT = 2, EVENT_SCAN_FAILED = 3, EVENT_CONNECTION = 4, EVENT_SERVICES = 5, EVENT_READ = 6,
            EVENT_WRITTEN = 7, EVENT_CHANGED = 8, EVENT_MTU = 9, EVENT_RSSI = 10, EVENT_BOND = 11, EVENT_PAIRING = 12;
    /* next()'s meta, by index. */
    public static final int KIND = 0, CONN = 1, ADDRESS = 2, ADDRESS_TYPE = 3, STATUS = 4, HANDLE = 5, VALUE = 6, EXTRA = 7, TIME = 8, META = 9;

    private Bluetooth() {}

    /** The radio: RADIO_*. The shell is asked the first time; changes come as events. */
    public static native int state();
    /** The radio's name, the phone's. */
    public static native String name();
    /** SCAN_*: the radio's scan, as the app's scans want it. */
    public static native boolean scan(int mode);
    /** A GATT connection to the device: conn 0 for a new one, else one made before; its number, 0 when there is no radio. */
    public static native int connect(long address, int type, boolean auto, int conn);
    /** The connection ended from this side; with close, forgotten. */
    public static native void disconnect(int conn, boolean close);
    public static native boolean discover(int conn);
    public static native boolean read(int conn, int handle);
    public static native boolean write(int conn, int handle, byte[] value, int type);
    public static native boolean mtu(int conn, int mtu);
    public static native boolean rssi(int conn);
    public static native boolean bond(long address, int type, boolean create);
    public static native void pairingReply(long address, int answer, String pin);
    /** The phone's Bluetooth settings shown, for the user to turn the radio on; false with no shell to show them. */
    public static native boolean settings();
    /** What the core knows of a device: its bond (BluetoothDevice.BOND_*), its address's kind (ADDRESS_*), its name or null. */
    public static native int bondState(long address);
    public static native int addressType(long address);
    public static native String deviceName(long address);
    /** The devices paired with the phone; those the app has a connection to. */
    public static native long[] bonded();
    public static native long[] connected();
    /**
     * The next event: its numbers into meta (META of them), and {byte[] data, String name, int[] attributes (kind, handle,
     * properties each), byte[] their UUIDs}; null when none came within timeoutMs.
     */
    public static native Object[] next(long[] meta, int timeoutMs);

    /** "AA:BB:CC:DD:EE:FF" as a number; -1 for text that is not an address. */
    public static long address(String text) {
        if (text == null || text.length() != 17) return -1;
        long v = 0;
        for (int i = 0; i < 17; i++) {
            char ch = text.charAt(i);
            if (i % 3 == 2) {
                if (ch != ':') return -1;
                continue;
            }
            int d = Character.digit(ch, 16);
            if (d < 0) return -1;
            v = v << 4 | d;
        }
        return v;
    }

    public static String text(long address) {
        StringBuilder b = new StringBuilder(17);
        for (int shift = 40; shift >= 0; shift -= 8) {
            int octet = (int) (address >> shift) & 0xff;
            b.append(Character.toUpperCase(Character.forDigit(octet >> 4, 16))).append(Character.toUpperCase(Character.forDigit(octet & 15, 16)));
            if (shift > 0) b.append(':');
        }
        return b.toString();
    }

    /** Android 12's runtime permission for an app that targets it (BLUETOOTH_SCAN, BLUETOOTH_CONNECT), else SecurityException. */
    public static void enforce(String permission, String what) {
        if (Manifest.targetSdk < 31) return;
        if (Permissions.check(permission) == Permissions.GRANTED) return;
        throw new SecurityException("Need " + permission + " permission for " + Native.packageName() + ": " + what);
    }

    /* An app's ACTION_REQUEST_ENABLE that waits for the radio: the activity that asked, and its request code. */
    private static android.app.Activity sEnableCaller;
    private static int sEnableRequest = -1;
    private static final android.os.Handler sMain = new android.os.Handler(android.os.Looper.getMainLooper());

    /**
     * Android's own screens about the radio, which an app starts (AppRunner.startActivity). ACTION_REQUEST_ENABLE, Android's
     * dialog that turns the radio on: Megler cannot turn it on, so the phone's Bluetooth settings are shown, and the result
     * is RESULT_OK once the radio is on, RESULT_CANCELED when the app is in front again with the radio still off -- RESULT_OK
     * at once when it is on already, as Android answers. Settings.ACTION_BLUETOOTH_SETTINGS shows the phone's settings.
     * ACTION_REQUEST_DISCOVERABLE (classic Bluetooth's) is refused. False for any other intent.
     */
    public static boolean request(final android.app.Activity caller, android.content.Intent i, final int request) {
        String action = i != null ? i.getAction() : null;
        if (android.bluetooth.BluetoothAdapter.ACTION_REQUEST_ENABLE.equals(action)) {
            ensure();
            if (state() == RADIO_ON) {
                Log.i(TAG, "asked to turn the radio on: it is on");
                result(caller, request, android.app.Activity.RESULT_OK);
                return true;
            }
            if (!settings()) {
                result(caller, request, android.app.Activity.RESULT_CANCELED);
                return true;
            }
            Log.i(TAG, "asked to turn the radio on: the phone's Bluetooth settings are shown, for the user to");
            synchronized (Bluetooth.class) {
                // (One that waited before has its answer now: no.)
                if (sEnableCaller != null) result(sEnableCaller, sEnableRequest, android.app.Activity.RESULT_CANCELED);
                sEnableCaller = request >= 0 ? caller : null;
                sEnableRequest = request;
            }
            return true;
        }
        if (android.provider.Settings.ACTION_BLUETOOTH_SETTINGS.equals(action)) {
            settings();
            result(caller, request, android.app.Activity.RESULT_CANCELED);
            return true;
        }
        if (android.bluetooth.BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE.equals(action)) {
            result(caller, request, android.app.Activity.RESULT_CANCELED);
            return true;
        }
        return false;
    }

    private static void result(final android.app.Activity caller, final int request, final int code) {
        if (caller == null || request < 0) return;
        sMain.post(() -> caller.meglerActivityResult(request, code, null));
    }

    /** The radio is on (BluetoothAdapter's broadcast): a request to turn it on that waits has RESULT_OK. */
    public static void radioOn() {
        android.app.Activity caller;
        int request;
        synchronized (Bluetooth.class) {
            caller = sEnableCaller;
            request = sEnableRequest;
            sEnableCaller = null;
        }
        result(caller, request, android.app.Activity.RESULT_OK);
    }

    /**
     * The app is in front again (AppRunner.hostPause): a request to turn the radio on that still waits has its answer a
     * moment later -- by when the radio's state, which may have changed meanwhile, has come -- RESULT_OK if it is on.
     */
    public static void resumed() {
        synchronized (Bluetooth.class) {
            if (sEnableCaller == null) return;
        }
        sMain.postDelayed(() -> {
            android.app.Activity caller;
            int request;
            synchronized (Bluetooth.class) {
                caller = sEnableCaller;
                request = sEnableRequest;
                sEnableCaller = null;
            }
            if (caller != null) result(caller, request, state() == RADIO_ON ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED);
        }, 1500);
    }

    private static Thread sThread;

    /** The thread that takes the radio's events, from the app's first use of Bluetooth on. */
    public static void ensure() {
        synchronized (Bluetooth.class) {
            if (sThread != null) return;
            sThread = new Thread(Bluetooth::loop, "BluetoothEventThread");
            sThread.setDaemon(true);
            sThread.start();
        }
    }

    private static void loop() {
        long[] meta = new long[META];
        for (;;) {
            Object[] e = next(meta, 1000);
            if (e == null) continue;
            try {
                dispatch(meta, e);
            } catch (Throwable t) {
                Log.e(TAG, "an event of the radio (" + meta[KIND] + ") could not be handed over", t);
            }
        }
    }

    private static void dispatch(long[] m, Object[] e) {
        int kind = (int) m[KIND];
        byte[] data = (byte[]) e[0];
        switch (kind) {
        case EVENT_STATE:
            android.bluetooth.BluetoothAdapter.meglerState((int) m[VALUE]);
            break;
        case EVENT_ADVERTISEMENT:
            android.bluetooth.le.BluetoothLeScanner.meglerHeard(m[ADDRESS], (int) m[ADDRESS_TYPE], (int) m[VALUE], m[EXTRA] != 0, data, m[TIME]);
            break;
        case EVENT_SCAN_FAILED:
            android.bluetooth.le.BluetoothLeScanner.meglerFailed((int) m[STATUS]);
            break;
        case EVENT_BOND:
            android.bluetooth.BluetoothDevice.meglerBond(m[ADDRESS], (int) m[ADDRESS_TYPE], (int) m[VALUE], (int) m[STATUS]);
            break;
        case EVENT_PAIRING:
            android.bluetooth.BluetoothDevice.meglerPairing(m[ADDRESS], (int) m[ADDRESS_TYPE], (int) m[VALUE], (int) m[EXTRA]);
            break;
        default:
            android.bluetooth.BluetoothGatt.meglerEvent(kind, (int) m[CONN], (int) m[STATUS], (int) m[HANDLE], (int) m[VALUE], data, (int[]) e[2], (byte[]) e[3]);
            break;
        }
    }
}
