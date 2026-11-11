package megler.test.bluetooth;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Bluetooth LE's central side as MeshCore's app uses it (its flutter_blue_plus), on the test's stand-in for the shell's radio
 * (core/tests/mg_bluetooth_standin.c): a radio with MeshCore-Test (the Nordic UART service: RX written, TX notified, what is
 * written echoed back; a characteristic it answers only once bonded, with the PIN 123456) and a beacon in range. A thread
 * of the app's goes through the steps and says how each went; what the radio does comes to callbacks, whose events wait
 * in a queue for the step that asked. While the radio is turned off by the test, the app waits for it.
 */
public class Main extends Activity {
    private static final String T = "BluetoothTest";
    private static final UUID NUS = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e"),
            TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e"), CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"),
            GAP = UUID.fromString("00001800-0000-1000-8000-00805f9b34fb"), NAME = UUID.fromString("00002a00-0000-1000-8000-00805f9b34fb"),
            SECRET_SERVICE = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb"), SECRET = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb"),
            BATTERY = UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb");
    private static final String MESHCORE = "C0:FF:EE:00:00:01", BEACON = "00:11:22:33:44:55", OLD = "11:22:33:44:55:66";
    /* Android 12's permissions and the bond's reason (hidden in the SDK the test is built with), by their names. */
    private static final String BLUETOOTH_SCAN = "android.permission.BLUETOOTH_SCAN", BLUETOOTH_CONNECT = "android.permission.BLUETOOTH_CONNECT",
            EXTRA_REASON = "android.bluetooth.device.extra.REASON";
    private int ok, failed;
    private String detail = "";
    private volatile int[] permissionResults;
    private volatile int enableResult = 1;
    private final CountDownLatch permissions = new CountDownLatch(1);
    private volatile CountDownLatch enabled = new CountDownLatch(1);
    private BluetoothAdapter adapter;
    private BluetoothManager manager;
    /* What the GATT callbacks, the scans and the receivers heard, in order, as text: "connection 0 2", "changed hello"... */
    private final BlockingQueue<String> heard = new LinkedBlockingQueue<>();
    /* Whether a pairing request is answered with the PIN (else the user is left to). */
    private volatile boolean answerPin = true;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Bluetooth");
        setContentView(v);
        new Thread(this::steps, "steps").start();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] names, int[] results) {
        if (request == 7) {
            permissionResults = results;
            permissions.countDown();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == 9) {
            enableResult = result;
            enabled.countDown();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }

    /* ------------------------------------------------------------ the steps */

    private interface Step { boolean run() throws Exception; }

    private void step(String name, Step s) {
        boolean good;
        detail = "";
        try {
            good = s.run();
        } catch (Throwable e) {
            Log.e(T, name + " threw", e);
            good = false;
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED " + detail));
    }

    /** The next thing heard that begins with `prefix`, within 30 seconds (qemu is slow); what came before it is said and dropped. */
    private String await(String prefix) throws InterruptedException {
        long until = System.currentTimeMillis() + 30000;
        for (;;) {
            long left = until - System.currentTimeMillis();
            String s = left > 0 ? heard.poll(left, TimeUnit.MILLISECONDS) : null;
            if (s == null) {
                detail += " (nothing like \"" + prefix + "\" heard)";
                return "";
            }
            if (s.startsWith(prefix)) return s;
            detail += " [" + s + "]";
        }
    }

    /**
     * The next `n` things heard, within 30 seconds each, as a list: for what comes on two threads at once (a GATT callback and
     * a broadcast), whose order between the threads is not the test's to know.
     */
    private List<String> collect(int n) throws InterruptedException {
        ArrayList<String> got = new ArrayList<>();
        while (got.size() < n) {
            String s = heard.poll(30, TimeUnit.SECONDS);
            if (s == null) break;
            got.add(s);
        }
        detail += " heard=" + got;
        return got;
    }

    /** Those of `got` that begin with `prefix`, in order. */
    private static List<String> only(List<String> got, String prefix) {
        ArrayList<String> out = new ArrayList<>();
        for (String s : got)
            if (s.startsWith(prefix)) out.add(s);
        return out;
    }

    private static String text(byte[] b) { return b == null ? "null" : new String(b, StandardCharsets.UTF_8); }
    private static boolean onMain() { return Looper.myLooper() == Looper.getMainLooper(); }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            heard.add("connection " + status + " " + newState + (onMain() ? " main" : ""));
        }
        @Override public void onServicesDiscovered(BluetoothGatt g, int status) { heard.add("services " + status + " " + g.getServices().size()); }
        @Override public void onMtuChanged(BluetoothGatt g, int mtu, int status) { heard.add("mtu " + mtu + " " + status); }
        @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            heard.add("descriptor-written " + d.getUuid().toString().substring(4, 8) + " " + status + " " + Arrays.toString(d.getValue()));
        }
        @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            heard.add("written " + Integer.toHexString(c.getInstanceId()) + " " + status);
        }
        // Android 13's form, with the value.
        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            heard.add("changed " + text(value) + " " + text(c.getValue()));
        }
        // The old form alone, as an app built before Android 13 has it: the new form's own calls it, the value in the characteristic.
        @Override public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            heard.add("read " + Integer.toHexString(c.getInstanceId()) + " " + status + " " + (status == 0 ? text(c.getValue()) : "-"));
        }
        @Override public void onReadRemoteRssi(BluetoothGatt g, int rssi, int status) { heard.add("rssi " + rssi + " " + status); }
    };

    private final BroadcastReceiver bonds = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            BluetoothDevice d = i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if (BluetoothDevice.ACTION_BOND_STATE_CHANGED.equals(i.getAction())) {
                heard.add("bond " + d.getAddress() + " " + i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1) + " " + i.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1)
                          + " " + i.getIntExtra(EXTRA_REASON, 0) + (onMain() ? " main" : ""));
            } else if (BluetoothDevice.ACTION_PAIRING_REQUEST.equals(i.getAction())) {
                int variant = i.getIntExtra(BluetoothDevice.EXTRA_PAIRING_VARIANT, -1);
                // As flutter_blue_plus answers it, with the PIN its app gave createBond.
                boolean set = answerPin && d.setPin("123456".getBytes(StandardCharsets.UTF_8));
                heard.add("pairing " + d.getAddress() + " " + variant + " " + (answerPin ? "pin=" + set : "unanswered"));
            } else if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(i.getAction())) {
                heard.add("state " + i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1) + " " + i.getIntExtra(BluetoothAdapter.EXTRA_PREVIOUS_STATE, -1));
            }
        }
    };

    private void steps() {
        // (Not Bluetooth's: this is the one test app made for Android 11 or later, and such an app has no android.test.base --
        // JUnit 3's junit.framework -- unless its manifest asks for it, as on Android since 11. An app made for Android 10 or
        // before has it: the data app's steps junit-*.)
        step("no-test-base", () -> {
            try {
                Class.forName("junit.framework.Assert");
                detail = "junit.framework.Assert is there";
                return false;
            } catch (ClassNotFoundException e) {
                return true;
            }
        });
        step("adapter", () -> {
            manager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
            adapter = manager.getAdapter();
            detail = "enabled=" + adapter.isEnabled() + " state=" + adapter.getState();
            return adapter == BluetoothAdapter.getDefaultAdapter() && adapter.isEnabled() && adapter.getState() == BluetoothAdapter.STATE_ON
                    && getPackageManager().hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE) && adapter.getBluetoothLeScanner() != null
                    && adapter.getBluetoothLeAdvertiser() == null && !adapter.isMultipleAdvertisementSupported()
                    && BluetoothAdapter.checkBluetoothAddress(MESHCORE) && !BluetoothAdapter.checkBluetoothAddress("c0:ff:ee:00:00:01");
        });
        step("permissions", () -> {
            // Android 12's: without them, what scans and connects is refused.
            requestPermissions(new String[] { BLUETOOTH_SCAN, BLUETOOTH_CONNECT }, 7);
            boolean answered = permissions.await(30, TimeUnit.SECONDS);
            int[] r = permissionResults;
            detail = "answered=" + answered + " results=" + Arrays.toString(r);
            return answered && r != null && r.length == 2 && r[0] == PackageManager.PERMISSION_GRANTED && r[1] == PackageManager.PERMISSION_GRANTED
                    && checkSelfPermission(BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
        });
        step("names", () -> {
            Set<BluetoothDevice> bonded = adapter.getBondedDevices();
            BluetoothDevice old = adapter.getRemoteDevice(OLD);
            detail = "name=" + adapter.getName() + " address=" + adapter.getAddress() + " bonded=" + bonded + " old=" + old.getName();
            return "Test Radio".equals(adapter.getName()) && "02:00:00:00:00:00".equals(adapter.getAddress()) && bonded.size() == 1 && bonded.contains(old)
                    && "Old Radio".equals(old.getName()) && old.getBondState() == BluetoothDevice.BOND_BONDED;
        });
        registerReceiver(bonds, filter(BluetoothDevice.ACTION_BOND_STATE_CHANGED, BluetoothDevice.ACTION_PAIRING_REQUEST, BluetoothAdapter.ACTION_STATE_CHANGED));
        step("scan", this::scan);
        step("scan-legacy", this::scanLegacy);
        final BluetoothDevice device = adapter.getRemoteDevice(MESHCORE);
        final BluetoothGatt[] gatt = new BluetoothGatt[1];
        step("connect", () -> {
            gatt[0] = device.connectGatt(this, false, callback, BluetoothDevice.TRANSPORT_LE);
            String c = await("connection ");
            List<BluetoothDevice> connected = manager.getConnectedDevices(BluetoothProfile.GATT);
            detail += " heard=" + c + " connected=" + connected;
            // (On a thread of the framework's, not the main one, as Android's binder thread.)
            return gatt[0] != null && "connection 0 2".equals(c) && connected.size() == 1 && connected.contains(device)
                    && manager.getConnectionState(device, BluetoothProfile.GATT) == BluetoothProfile.STATE_CONNECTED && gatt[0].getDevice().equals(device);
        });
        step("discover", () -> {
            boolean asked = gatt[0].discoverServices();
            String s = await("services ");
            BluetoothGattService nus = gatt[0].getService(NUS);
            BluetoothGattCharacteristic rx = nus != null ? nus.getCharacteristic(RX) : null, tx = nus != null ? nus.getCharacteristic(TX) : null;
            BluetoothGattDescriptor cccd = tx != null ? tx.getDescriptor(CCCD) : null;
            BluetoothGattService gap = gatt[0].getService(GAP);
            detail += " asked=" + asked + " heard=" + s + " nus=" + (nus != null) + " rx=" + (rx != null ? rx.getProperties() + "/" + rx.getInstanceId() + "/" + rx.getWriteType() : null)
                      + " tx=" + (tx != null ? tx.getProperties() + "/" + tx.getInstanceId() : null) + " cccd=" + (cccd != null);
            return asked && "services 0 3".equals(s) && nus.getType() == BluetoothGattService.SERVICE_TYPE_PRIMARY && nus.getInstanceId() == 0x10
                    && rx.getProperties() == (BluetoothGattCharacteristic.PROPERTY_WRITE | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE)
                    && rx.getInstanceId() == 0x12 && rx.getWriteType() == BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE && rx.getService() == nus
                    && tx.getProperties() == BluetoothGattCharacteristic.PROPERTY_NOTIFY && tx.getInstanceId() == 0x14 && cccd != null
                    && cccd.getCharacteristic() == tx && gap != null && gap.getCharacteristic(NAME) != null
                    && gatt[0].getService(UUID.fromString("0000feed-0000-1000-8000-00805f9b34fb")) == null;
        });
        step("mtu", () -> gatt[0].requestMtu(247) && "mtu 185 0".equals(await("mtu ")));
        step("notify", () -> {
            BluetoothGattCharacteristic tx = gatt[0].getService(NUS).getCharacteristic(TX);
            BluetoothGattDescriptor cccd = tx.getDescriptor(CCCD);
            // As flutter_blue_plus's setNotifyValue: the characteristic let through, then its descriptor written with Android 13's call.
            boolean let = gatt[0].setCharacteristicNotification(tx, true);
            int code = gatt[0].writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            String w = await("descriptor-written ");
            detail += " let=" + let + " code=" + code + " heard=" + w;
            return let && code == BluetoothStatusCodes.SUCCESS && "descriptor-written 2902 0 [1, 0]".equals(w);
        });
        step("write", () -> {
            BluetoothGattCharacteristic rx = gatt[0].getService(NUS).getCharacteristic(RX);
            int code = gatt[0].writeCharacteristic(rx, "hello".getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            String w = await("written "), n = await("changed ");
            detail += " code=" + code + " heard=" + w + " / " + n;
            return code == BluetoothStatusCodes.SUCCESS && "written 12 0".equals(w) && "changed hello hello".equals(n);
        });
        step("write-no-response", () -> {
            BluetoothGattCharacteristic rx = gatt[0].getService(NUS).getCharacteristic(RX);
            int code = gatt[0].writeCharacteristic(rx, "abc".getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            String w = await("written "), n = await("changed ");
            detail += " code=" + code + " heard=" + w + " / " + n;
            return code == BluetoothStatusCodes.SUCCESS && "written 12 0".equals(w) && "changed abc abc".equals(n);
        });
        step("write-old", () -> {
            // The old form: the characteristic's own value and write type.
            BluetoothGattCharacteristic rx = gatt[0].getService(NUS).getCharacteristic(RX);
            rx.setValue("xyz");
            rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            boolean sent = gatt[0].writeCharacteristic(rx);
            String w = await("written "), n = await("changed ");
            detail += " sent=" + sent + " heard=" + w + " / " + n;
            return sent && "written 12 0".equals(w) && "changed xyz xyz".equals(n);
        });
        step("read", () -> {
            BluetoothGattCharacteristic name = gatt[0].getService(GAP).getCharacteristic(NAME);
            // One request at a time: the second, while the first is answered, refused.
            boolean first = gatt[0].readCharacteristic(name), second = gatt[0].readCharacteristic(name);
            String r = await("read ");
            detail += " first=" + first + " second=" + second + " heard=" + r;
            return first && !second && "read 3 0 MeshCore-Test".equals(r);
        });
        step("rssi", () -> gatt[0].readRemoteRssi() && "rssi -50 0".equals(await("rssi ")));
        step("bond-on-demand", () -> {
            // The device answers this one only once bonded: the radio pairs (the app hears the bond change and the PIN asked
            // for, which it gives), then the read is sent again and answered.
            BluetoothGattCharacteristic secret = gatt[0].getService(SECRET_SERVICE).getCharacteristic(SECRET);
            boolean asked = gatt[0].readCharacteristic(secret);
            List<String> got = collect(4);
            detail += " asked=" + asked;
            return asked && Arrays.asList("bond " + MESHCORE + " 11 10 0 main", "bond " + MESHCORE + " 12 11 0 main").equals(only(got, "bond "))
                    && got.contains("pairing " + MESHCORE + " 0 pin=true") && got.indexOf("pairing " + MESHCORE + " 0 pin=true") < got.indexOf("bond " + MESHCORE + " 12 11 0 main")
                    && got.contains("read 22 0 s3cret") && device.getBondState() == BluetoothDevice.BOND_BONDED && adapter.getBondedDevices().contains(device);
        });
        step("unbond", () -> {
            // As flutter_blue_plus removes a bond: Android's hidden removeBond, by reflection.
            boolean removed = (Boolean) device.getClass().getMethod("removeBond").invoke(device);
            String b = await("bond ");
            detail += " removed=" + removed + " heard=" + b;
            return removed && ("bond " + MESHCORE + " 10 12 9 main").equals(b) && device.getBondState() == BluetoothDevice.BOND_NONE;
        });
        step("bond-asked", () -> {
            // createBond, and the PIN asked for left unanswered: the user is asked, as by Android's own pairing dialog.
            answerPin = false;
            boolean started = device.createBond();
            String b1 = await("bond "), p = await("pairing "), b2 = await("bond ");
            answerPin = true;
            boolean again = device.createBond();
            detail += " started=" + started + " again=" + again + " heard=" + b1 + " / " + p + " / " + b2;
            return started && !again && ("bond " + MESHCORE + " 11 10 0 main").equals(b1) && ("pairing " + MESHCORE + " 0 unanswered").equals(p)
                    && ("bond " + MESHCORE + " 12 11 0 main").equals(b2);
        });
        step("disconnect", () -> {
            gatt[0].disconnect();
            String c = await("connection ");
            gatt[0].close();
            detail += " heard=" + c;
            return "connection 0 0".equals(c) && manager.getConnectedDevices(BluetoothProfile.GATT).isEmpty();
        });
        step("dropped", () -> {
            // The device ends the connection: Android's 19, the remote user terminated it.
            BluetoothGatt g = device.connectGatt(this, false, callback);
            boolean made = "connection 0 2".equals(await("connection "));
            boolean found = g.discoverServices() && "services 0 3".equals(await("services "));
            int code = g.writeCharacteristic(g.getService(NUS).getCharacteristic(RX), "drop".getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            String w = await("written "), c = await("connection ");
            g.close();
            detail += " made=" + made + " found=" + found + " code=" + code + " heard=" + w + " / " + c;
            return made && found && code == 0 && "written 12 0".equals(w) && "connection 19 0".equals(c);
        });
        step("unreachable", () -> {
            // A device that never answers: 133, as Android's.
            BluetoothGatt g = adapter.getRemoteDevice("02:00:00:00:00:09").connectGatt(this, false, callback);
            String c = await("connection ");
            g.close();
            detail += " heard=" + c;
            return "connection 133 0".equals(c);
        });
        step("radio-off", () -> {
            Log.i(T, "turn the radio off");
            String s1 = await("state "), s2 = await("state ");
            detail += " heard=" + s1 + " / " + s2 + " enabled=" + adapter.isEnabled();
            return "state 13 12".equals(s1) && "state 10 13".equals(s2) && !adapter.isEnabled() && adapter.getBluetoothLeScanner() == null
                    && device.connectGatt(this, false, callback) == null;
        });
        step("enable", () -> {
            // Android's dialog that turns the radio on: the phone's settings shown, where (the stand-in's) user turns it on.
            runOnUiThread(() -> startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), 9));
            String s1 = await("state "), s2 = await("state ");
            boolean answered = enabled.await(30, TimeUnit.SECONDS);
            int first = enableResult;
            // And asked again while it is on: RESULT_OK at once, as Android answers.
            enabled = new CountDownLatch(1);
            runOnUiThread(() -> startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), 9));
            boolean again = enabled.await(30, TimeUnit.SECONDS);
            detail += " heard=" + s1 + " / " + s2 + " result=" + first + " again=" + enableResult;
            return "state 11 10".equals(s1) && "state 12 11".equals(s2) && answered && first == Activity.RESULT_OK && again && enableResult == Activity.RESULT_OK
                    && adapter.isEnabled();
        });
        unregisterReceiver(bonds);
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }

    private static IntentFilter filter(String... actions) {
        IntentFilter f = new IntentFilter();
        for (String a : actions) f.addAction(a);
        return f;
    }

    /**
     * A scan for the Nordic UART service, as MeshCore's app looks for its radios: MeshCore-Test is heard, on the main thread,
     * with every field of its advertisement read as Android reads it; the beacon, which has not that service, is not. The same
     * callback started again is told it is started already.
     */
    private boolean scan() throws Exception {
        final BlockingQueue<Object> results = new LinkedBlockingQueue<>();
        ScanCallback cb = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult r) { results.add(onMain() ? r : "not on the main thread"); }
            @Override public void onScanFailed(int code) { results.add("failed " + code); }
        };
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        List<ScanFilter> filters = Collections.singletonList(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(NUS)).build());
        scanner.startScan(filters, new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb);
        scanner.startScan(filters, new ScanSettings.Builder().build(), cb);
        boolean refused = false, meshcore = false, beacon = false, other = false;
        long until = System.currentTimeMillis() + 30000;
        int seen = 0;
        while (System.currentTimeMillis() < until && seen < 4) {
            Object o = results.poll(until - System.currentTimeMillis(), TimeUnit.MILLISECONDS);
            if (o == null) break;
            if (("failed " + ScanCallback.SCAN_FAILED_ALREADY_STARTED).equals(o)) {
                refused = true;
                continue;
            }
            if (!(o instanceof ScanResult)) {
                other = true;
                detail += " " + o;
                continue;
            }
            ScanResult r = (ScanResult) o;
            seen++;
            if (BEACON.equals(r.getDevice().getAddress())) beacon = true;
            if (!MESHCORE.equals(r.getDevice().getAddress()) || meshcore) continue;
            ScanRecord rec = r.getScanRecord();
            byte[] name = rec.getAdvertisingDataMap().get(ScanRecordTypes.LOCAL_NAME_COMPLETE);
            meshcore = r.getRssi() == -50 && r.isConnectable() && r.isLegacy() && "MeshCore-Test".equals(rec.getDeviceName())
                       && rec.getServiceUuids() != null && rec.getServiceUuids().contains(new ParcelUuid(NUS)) && rec.getAdvertiseFlags() == 6
                       && Arrays.equals(rec.getManufacturerSpecificData(0xFFFF), new byte[] { 1, 2, 3 }) && rec.getTxPowerLevel() == -4
                       && name != null && "MeshCore-Test".equals(text(name)) && rec.getBytes() != null && rec.getBytes().length == 46
                       && "MeshCore-Test".equals(r.getDevice().getName()) && r.getTimestampNanos() > 0;
            if (!meshcore) detail += " meshcore=" + r;
        }
        scanner.stopScan(cb);
        detail += " refused=" + refused + " meshcore=" + meshcore + " beacon=" + beacon + " other=" + other;
        return refused && meshcore && !beacon && !other;
    }

    /** Android 4.3's scan: both devices, with their raw records, on the main thread. */
    private boolean scanLegacy() throws Exception {
        final BlockingQueue<String> results = new LinkedBlockingQueue<>();
        BluetoothAdapter.LeScanCallback cb = (d, rssi, record) -> results.add(d.getAddress() + " " + rssi + " " + record.length + (onMain() ? "" : " off-main"));
        boolean started = adapter.startLeScan(cb);
        boolean meshcore = false, beacon = false;
        long until = System.currentTimeMillis() + 30000;
        while (System.currentTimeMillis() < until && !(meshcore && beacon)) {
            String s = results.poll(until - System.currentTimeMillis(), TimeUnit.MILLISECONDS);
            if (s == null) break;
            if (s.equals(MESHCORE + " -50 46")) meshcore = true;
            else if (s.equals(BEACON + " -70 20")) beacon = true;
            else detail += " [" + s + "]";
        }
        adapter.stopLeScan(cb);
        detail += " started=" + started + " meshcore=" + meshcore + " beacon=" + beacon;
        return started && meshcore && beacon;
    }

    /** The AD types the test reads by number (Android 13's ScanRecord.DATA_TYPE_*). */
    private static final class ScanRecordTypes {
        static final int LOCAL_NAME_COMPLETE = 0x09;
    }
}
